// Terreno infinito gerado proceduralmente.
//
// O terreno oscila em torno de uma linha de altura constante (y = 0), com
// morros e rampas irregulares que variam tanto em x quanto em z: o jogador
// pode escolher seu caminho entre eles. Sua altura é a soma de algumas
// superfícies de Bézier bicúbicas por partes (produto tensorial de duas
// curvas de Bézier cúbicas), em escalas diferentes. Veja o documento
// Aula_16_Curvas_e_Superficies.pdf.
//
// O terreno é uma faixa retangular: infinito em x, mas com largura
// TERRAIN_WIDTH em z (centrada em z = 0). O jogador que sair da faixa cai.
//
// Para que o terreno seja "infinito", apenas TERRAIN_NUM_CHUNKS pedaços
// (chunks) existem na GPU ao mesmo tempo, cada um cobrindo toda a largura da
// faixa. Conforme o jogador avança em +x, os chunks que ficaram para trás são
// reaproveitados: seus vértices são recalculados para a nova posição à frente
// e reenviados para a GPU.

#include <cmath>
#include <vector>

#include <glad/glad.h>

// Camadas (superfícies de Bézier) cuja soma forma o terreno. Cada uma tem
// retalhos de um tamanho, alturas sorteadas para os vértices da grade, e uma
// grade girada por um ângulo diferente, para que os morros não fiquem
// alinhados aos eixos nem entre si. Veja AccumulateSurfaceLayer().
//
// Em uma camada comum, as alturas dos vértices são sorteadas em
// [-amplitude, +amplitude]. Em uma camada de montanhas (mountain_probability
// > 0), a maioria dos vértices tem altura 0, e apenas uma fração deles,
// sorteada, recebe uma altura grande, em [0.6, 1] * amplitude: são picos
// isolados e íngremes no meio do relevo ondulado.
struct TerrainLayer
{
    float patch_size;           // Lado de cada retalho
    float amplitude;            // Escala das alturas dos vértices da grade
    float angle;                // Rotação da grade em torno do eixo y, em radianos
    float mountain_probability; // 0 para uma camada comum; senão, probabilidade de um vértice ser um pico
};

static const TerrainLayer g_TerrainLayers[] =
{
    { 40.0f, 2.0f,  1.1f, 0.0f  }, // Variações largas: trechos mais altos e mais baixos
    { 16.0f, 3.0f,  0.4f, 0.0f  }, // Morros e rampas
    {  7.0f, 0.8f, -0.7f, 0.0f  }, // Ondulações menores sobre os morros
    { 11.0f, 9.0f,  0.8f, 0.12f }, // Montanhas íngremes, esparsas
};
static const int g_NumTerrainLayers = sizeof(g_TerrainLayers) / sizeof(g_TerrainLayers[0]);

// Largura da faixa em z
#define TERRAIN_WIDTH          48.0f

// Parâmetros da malha de cada chunk
#define TERRAIN_NUM_CHUNKS     10     // Número de chunks existentes ao mesmo tempo
#define TERRAIN_CHUNKS_BEHIND  4      // Quantos deles ficam atrás do jogador
#define TERRAIN_CHUNK_LENGTH   16.0f  // Comprimento de cada chunk em x
#define TERRAIN_QUADS_X        32     // Resolução da grade de cada chunk
#define TERRAIN_QUADS_Z        96
#define TERRAIN_TEXTURE_SCALE  0.25f  // Repetições da textura por unidade do mundo

// Número de vértices de cada chunk
#define TERRAIN_VERTS_X (TERRAIN_QUADS_X + 1)
#define TERRAIN_VERTS_Z (TERRAIN_QUADS_Z + 1)
#define TERRAIN_NUM_VERTICES (TERRAIN_VERTS_X * TERRAIN_VERTS_Z)

struct TerrainChunk
{
    int    chunk_index; // Qual trecho do terreno este chunk representa (x inicial = chunk_index * TERRAIN_CHUNK_LENGTH)
    GLuint vertex_array_object_id;
    GLuint VBO_model_coefficients_id;
    GLuint VBO_normal_coefficients_id;
    GLuint VBO_texture_coefficients_id;
};

static TerrainChunk g_TerrainChunks[TERRAIN_NUM_CHUNKS];
static GLsizei g_TerrainNumIndices = 0;

// Semente do gerador de alturas: cada semente gera um terreno diferente.
// Veja Terrain_Reset().
static unsigned int g_TerrainSeed = 0;

// Número pseudoaleatório em [0,1], porém determinístico, associado ao
// vértice (i,j) da grade da camada "layer". Por ser determinístico, a mesma
// posição sempre gera a mesma altura, o que permite gerar qualquer trecho do
// terreno a qualquer momento sem emendas.
static float Random(int i, int j, unsigned int layer)
{
    unsigned int h = (unsigned int)i * 374761393u + (unsigned int)j * 668265263u
                   + g_TerrainSeed * 2246822519u + layer * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (h & 0xFFFF) / 65535.0f;
}

// Polinômios de Bernstein de grau 3, B[k](t), e suas derivadas dB[k](t).
static void Bernstein(float t, float B[4], float dB[4])
{
    float s = 1.0f - t;
    B[0] = s*s*s;
    B[1] = 3.0f*t*s*s;
    B[2] = 3.0f*t*t*s;
    B[3] = t*t*t;
    dB[0] = -3.0f*s*s;
    dB[1] =  3.0f*s*s - 6.0f*t*s;
    dB[2] =  6.0f*t*s - 3.0f*t*t;
    dB[3] =  3.0f*t*t;
}

// Altura sorteada para o vértice (i,j) da grade da camada "layer".
static float VertexHeight(int i, int j, int layer)
{
    const TerrainLayer& params = g_TerrainLayers[layer];
    float r = Random(i, j, layer);

    if (params.mountain_probability <= 0.0f)
        return params.amplitude * (2.0f * r - 1.0f);

    if (r >= params.mountain_probability)
        return 0.0f;
    return params.amplitude * (0.6f + 0.4f * Random(i, j, layer + 100));
}

// Avalia, na posição (x,z) do mundo, a camada "layer" do terreno, e acumula
// sua altura e suas derivadas parciais dh/dx e dh/dz em h, dhdx e dhdz.
//
// A camada é uma superfície de Bézier bicúbica por partes sobre uma grade de
// retalhos quadrados, girada pelo ângulo da camada. Cada vértice (i,j) da
// grade tem uma altura H sorteada. As derivadas da superfície nos vértices
// são estimadas por diferenças centrais entre os vizinhos:
//
//     dH/du = (H(i+1,j) - H(i-1,j)) / 2,   dH/dv = (H(i,j+1) - H(i,j-1)) / 2
//
// (em unidades do parâmetro, que vai de 0 a 1 em cada retalho). Os pontos de
// controle de cada retalho são então posicionados a partir dos cantos:
//
//   - P[0][0] = H no canto;
//   - P[1][0] = H + (dH/du)/3 e P[0][1] = H + (dH/dv)/3, pois a derivada de
//     uma curva de Bézier no início é 3(P1 - P0);
//   - P[1][1] = H + (dH/du)/3 + (dH/dv)/3 (derivada mista nula);
//
// e analogamente, com os sinais trocados, nos outros três cantos. Como dois
// retalhos vizinhos usam a mesma altura e as mesmas derivadas nos vértices
// em comum, a superfície tem continuidade C1: não há quinas. E, ao contrário
// de fazer P1 = P0, as derivadas nos vértices não são nulas, então a
// superfície não fica "achatada" ao longo das linhas da grade.
static void AccumulateSurfaceLayer(float x, float z, int layer, float* h, float* dhdx, float* dhdz)
{
    const TerrainLayer& params = g_TerrainLayers[layer];
    float L = params.patch_size;

    // Coordenadas (x,z) no sistema da grade girada.
    float c  = cosf(params.angle);
    float sn = sinf(params.angle);
    float xr =  c*x + sn*z;
    float zr = -sn*x + c*z;

    float su = xr / L;
    float sv = zr / L;
    int   i  = (int)floorf(su);
    int   j  = (int)floorf(sv);
    float u  = su - i;
    float v  = sv - j;

    // Alturas dos vértices da grade ao redor do retalho: H[a][b] é o vértice
    // (i-1+a, j-1+b).
    float H[4][4];
    for (int a = 0; a < 4; ++a)
        for (int b = 0; b < 4; ++b)
            H[a][b] = VertexHeight(i - 1 + a, j - 1 + b, layer);

    // Pontos de controle do retalho
    float P[4][4];
    for (int a = 0; a < 2; ++a)
    {
        for (int b = 0; b < 2; ++b)
        {
            int   ga = 1 + a, gb = 1 + b;  // Canto (a,b) do retalho em H
            float corner = H[ga][gb];
            float du = (H[ga + 1][gb] - H[ga - 1][gb]) / 2.0f;
            float dv = (H[ga][gb + 1] - H[ga][gb - 1]) / 2.0f;
            float su_sign = (a == 0) ? 1.0f : -1.0f; // Para dentro do retalho
            float sv_sign = (b == 0) ? 1.0f : -1.0f;
            int   k  = 3*a, l  = 3*b;               // Índices do canto
            int   k1 = (a == 0) ? 1 : 2, l1 = (b == 0) ? 1 : 2; // Vizinhos internos

            P[k ][l ] = corner;
            P[k1][l ] = corner + su_sign * du / 3.0f;
            P[k ][l1] = corner + sv_sign * dv / 3.0f;
            P[k1][l1] = corner + su_sign * du / 3.0f + sv_sign * dv / 3.0f;
        }
    }

    float Bu[4], dBu[4], Bv[4], dBv[4];
    Bernstein(u, Bu, dBu);
    Bernstein(v, Bv, dBv);

    float sum = 0.0f, sum_du = 0.0f, sum_dv = 0.0f;
    for (int k = 0; k < 4; ++k)
    {
        for (int l = 0; l < 4; ++l)
        {
            sum    +=  Bu[k] *  Bv[l] * P[k][l];
            sum_du += dBu[k] *  Bv[l] * P[k][l];
            sum_dv +=  Bu[k] * dBv[l] * P[k][l];
        }
    }

    // Pela regra da cadeia, as derivadas em relação às coordenadas giradas
    // são dh/dxr = dh/du / L e dh/dzr = dh/dv / L, e as derivadas em relação
    // a x e z do mundo são obtidas desfazendo a rotação.
    float dhdxr = sum_du / L;
    float dhdzr = sum_dv / L;
    *h    += sum;
    *dhdx += c*dhdxr - sn*dhdzr;
    *dhdz += sn*dhdxr + c*dhdzr;
}

// Avalia o terreno na posição (x,z) do mundo: altura h e derivadas parciais
// dh/dx e dh/dz. O terreno é a soma das camadas. A soma de superfícies C1
// também é C1, e a derivada da soma é a soma das derivadas.
static void EvaluateSurface(float x, float z, float* h, float* dhdx, float* dhdz)
{
    float height = 0.0f, gx = 0.0f, gz = 0.0f;
    for (int layer = 0; layer < g_NumTerrainLayers; ++layer)
        AccumulateSurfaceLayer(x, z, layer, &height, &gx, &gz);

    *h = height;
    if (dhdx) *dhdx = gx;
    if (dhdz) *dhdz = gz;
}

// Altura y do terreno na posição (x,z) do mundo.
float TerrainHeight(float x, float z)
{
    float h;
    EvaluateSurface(x, z, &h, NULL, NULL);
    return h;
}

// Gradiente (dh/dx, dh/dz) do terreno na posição (x,z) do mundo.
void TerrainGradient(float x, float z, float* dhdx, float* dhdz)
{
    float h;
    EvaluateSurface(x, z, &h, dhdx, dhdz);
}

// Metade da largura da faixa: o terreno existe apenas para |z| <= este valor.
float TerrainHalfWidth()
{
    return TERRAIN_WIDTH / 2.0f;
}

// Recalcula os vértices do chunk para representar o trecho chunk_index do
// terreno, e envia os novos dados para a GPU.
static void FillChunk(TerrainChunk& chunk, int chunk_index)
{
    std::vector<float> model_coefficients;
    std::vector<float> normal_coefficients;
    std::vector<float> texture_coefficients;
    model_coefficients.reserve(4 * TERRAIN_NUM_VERTICES);
    normal_coefficients.reserve(4 * TERRAIN_NUM_VERTICES);
    texture_coefficients.reserve(2 * TERRAIN_NUM_VERTICES);

    float x_start = chunk_index * TERRAIN_CHUNK_LENGTH;

    for (int i = 0; i < TERRAIN_VERTS_X; ++i)
    {
        float x = x_start + TERRAIN_CHUNK_LENGTH * i / TERRAIN_QUADS_X;

        for (int j = 0; j < TERRAIN_VERTS_Z; ++j)
        {
            float z = -TERRAIN_WIDTH/2.0f + TERRAIN_WIDTH * j / TERRAIN_QUADS_Z;

            float y, dhdx, dhdz;
            EvaluateSurface(x, z, &y, &dhdx, &dhdz);

            model_coefficients.push_back( x );    // X
            model_coefficients.push_back( y );    // Y
            model_coefficients.push_back( z );    // Z
            model_coefficients.push_back( 1.0f ); // W

            // A normal de uma superfície y = h(x,z) é (-dh/dx, 1, -dh/dz), normalizada.
            float len = sqrtf(dhdx*dhdx + 1.0f + dhdz*dhdz);
            normal_coefficients.push_back( -dhdx / len ); // X
            normal_coefficients.push_back(  1.0f / len ); // Y
            normal_coefficients.push_back( -dhdz / len ); // Z
            normal_coefficients.push_back(  0.0f );       // W

            // Coordenadas de textura computadas a partir da posição no mundo,
            // para que a textura continue sem costuras de um chunk para o
            // próximo. Requer GL_REPEAT no sampler (veja LoadTextureImage()).
            texture_coefficients.push_back( x * TERRAIN_TEXTURE_SCALE );
            texture_coefficients.push_back( z * TERRAIN_TEXTURE_SCALE );
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, chunk.VBO_model_coefficients_id);
    glBufferSubData(GL_ARRAY_BUFFER, 0, model_coefficients.size() * sizeof(float), model_coefficients.data());
    glBindBuffer(GL_ARRAY_BUFFER, chunk.VBO_normal_coefficients_id);
    glBufferSubData(GL_ARRAY_BUFFER, 0, normal_coefficients.size() * sizeof(float), normal_coefficients.data());
    glBindBuffer(GL_ARRAY_BUFFER, chunk.VBO_texture_coefficients_id);
    glBufferSubData(GL_ARRAY_BUFFER, 0, texture_coefficients.size() * sizeof(float), texture_coefficients.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    chunk.chunk_index = chunk_index;
}

// Cria um VBO com espaço para num_floats floats e o associa ao atributo
// "location" do Vertex Shader. O VAO correspondente deve estar ligado.
static GLuint CreateAttributeBuffer(GLuint location, GLint number_of_dimensions, size_t num_floats)
{
    GLuint VBO_id;
    glGenBuffers(1, &VBO_id);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_id);
    // GL_DYNAMIC_DRAW: os dados serão modificados sempre que o chunk for reaproveitado
    glBufferData(GL_ARRAY_BUFFER, num_floats * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(location, number_of_dimensions, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(location);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return VBO_id;
}

// Cria os VAOs e VBOs de todos os chunks. Deve ser chamada uma vez, na
// inicialização, depois de o contexto OpenGL ter sido criado.
void Terrain_Init()
{
    // Os índices são os mesmos para todos os chunks, já que a topologia da
    // grade nunca muda: cada quad da grade é formado por dois triângulos,
    // com vértices em sentido anti-horário quando vistos de cima (+y), de
    // forma que não sejam descartados pelo backface culling.
    std::vector<GLuint> indices;
    for (int i = 0; i < TERRAIN_QUADS_X; ++i)
    {
        for (int j = 0; j < TERRAIN_QUADS_Z; ++j)
        {
            GLuint a = i*TERRAIN_VERTS_Z + j;   // (i  , j  )
            GLuint b = a + 1;                   // (i  , j+1)
            GLuint c = a + TERRAIN_VERTS_Z;     // (i+1, j  )
            GLuint d = c + 1;                   // (i+1, j+1)

            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(c); indices.push_back(b); indices.push_back(d);
        }
    }
    g_TerrainNumIndices = (GLsizei)indices.size();

    // Como o padrão de índices é o mesmo, ele é enviado para a GPU uma única
    // vez, e o mesmo buffer é associado a todos os VAOs.
    GLuint indices_id;
    glGenBuffers(1, &indices_id);

    for (int k = 0; k < TERRAIN_NUM_CHUNKS; ++k)
    {
        TerrainChunk& chunk = g_TerrainChunks[k];

        glGenVertexArrays(1, &chunk.vertex_array_object_id);
        glBindVertexArray(chunk.vertex_array_object_id);

        // Mesmos "location" utilizados em "shader_vertex.glsl"
        chunk.VBO_model_coefficients_id   = CreateAttributeBuffer(0, 4, 4 * TERRAIN_NUM_VERTICES);
        chunk.VBO_normal_coefficients_id  = CreateAttributeBuffer(1, 4, 4 * TERRAIN_NUM_VERTICES);
        chunk.VBO_texture_coefficients_id = CreateAttributeBuffer(2, 2, 2 * TERRAIN_NUM_VERTICES);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_id);
        if (k == 0)
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);

        // Índice inválido: força o preenchimento na primeira chamada de Terrain_Update()
        chunk.chunk_index = -1000000;
    }
}

// Troca o terreno por um novo, gerado com outra semente. Os chunks são
// marcados como inválidos e recalculados na próxima chamada de
// Terrain_Update().
void Terrain_Reset(unsigned int seed)
{
    g_TerrainSeed = seed;
    for (int k = 0; k < TERRAIN_NUM_CHUNKS; ++k)
        g_TerrainChunks[k].chunk_index = -1000000;
}

// Garante que os chunks cubram o trecho do terreno ao redor da posição x do
// jogador: alguns chunks atrás dele e os demais à frente. Deve ser chamada a
// cada quadro, antes de Terrain_Draw().
void Terrain_Update(float player_x)
{
    int first = (int)floorf(player_x / TERRAIN_CHUNK_LENGTH) - TERRAIN_CHUNKS_BEHIND;

    for (int chunk_index = first; chunk_index < first + TERRAIN_NUM_CHUNKS; ++chunk_index)
    {
        // Cada trecho do terreno sempre ocupa a mesma posição do vetor
        // (buffer circular). O chunk que estava nesta posição representa um
        // trecho que ficou para trás, e é reaproveitado.
        int slot = ((chunk_index % TERRAIN_NUM_CHUNKS) + TERRAIN_NUM_CHUNKS) % TERRAIN_NUM_CHUNKS;

        if (g_TerrainChunks[slot].chunk_index != chunk_index)
            FillChunk(g_TerrainChunks[slot], chunk_index);
    }
}

// Desenha todos os chunks do terreno. As coordenadas dos vértices já estão
// no sistema de coordenadas global, então a matriz "model" deve ser a
// identidade.
void Terrain_Draw()
{
    for (int k = 0; k < TERRAIN_NUM_CHUNKS; ++k)
    {
        glBindVertexArray(g_TerrainChunks[k].vertex_array_object_id);
        glDrawElements(GL_TRIANGLES, g_TerrainNumIndices, GL_UNSIGNED_INT, (void*)0);
    }
    glBindVertexArray(0);
}
