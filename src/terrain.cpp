// Terreno infinito gerado proceduralmente.
//
// O perfil do terreno (altura y ao longo de x) é uma curva de Bézier cúbica
// por partes: cada segmento liga o topo de um morro ao fundo de um vale (ou
// vice-versa). A superfície é a extrusão desse perfil ao longo do eixo z.
// Veja o documento Aula_16_Curvas_e_Superficies.pdf.
//
// Para que o terreno seja "infinito", apenas TERRAIN_NUM_CHUNKS pedaços
// (chunks) existem na GPU ao mesmo tempo. Conforme o jogador avança em +x, os
// chunks que ficaram para trás são reaproveitados: seus vértices são
// recalculados para a nova posição à frente e reenviados para a GPU.

#include <cmath>
#include <vector>

#include <glad/glad.h>
#include <glm/vec4.hpp>

// Parâmetros da curva que define o perfil do terreno
#define TERRAIN_SEGMENT_LENGTH 8.0f  // Distância em x entre um topo e o vale seguinte
#define TERRAIN_PEAK_MIN       2.0f  // Faixa de alturas dos topos dos morros
#define TERRAIN_PEAK_MAX       4.0f
#define TERRAIN_VALLEY_MIN    -1.0f  // Faixa de alturas dos fundos dos vales
#define TERRAIN_VALLEY_MAX     0.5f

// Parâmetros da malha de cada chunk
#define TERRAIN_NUM_CHUNKS     8      // Número de chunks existentes ao mesmo tempo
#define TERRAIN_CHUNK_LENGTH   16.0f  // Comprimento de cada chunk em x
#define TERRAIN_WIDTH          20.0f  // Largura do terreno em z (centrado em z = 0)
#define TERRAIN_QUADS_X        64     // Resolução da grade de cada chunk
#define TERRAIN_QUADS_Z        20
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

// Altura pseudoaleatória, porém determinística, do ponto de controle i do
// perfil. Pontos pares são topos de morros e pontos ímpares são vales. Por ser
// determinística, a mesma posição x sempre gera a mesma altura, o que permite
// gerar qualquer trecho do terreno a qualquer momento sem emendas.
static float ControlPointHeight(int i)
{
    unsigned int h = (unsigned int)i * 374761393u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    float r = (h & 0xFFFF) / 65535.0f; // r em [0,1]

    if (i % 2 == 0)
        return TERRAIN_PEAK_MIN + r * (TERRAIN_PEAK_MAX - TERRAIN_PEAK_MIN);
    else
        return TERRAIN_VALLEY_MIN + r * (TERRAIN_VALLEY_MAX - TERRAIN_VALLEY_MIN);
}

// Encontra o segmento de Bézier que contém a posição x e computa as alturas
// dos seus quatro pontos de controle P0, P1, P2, P3, e o parâmetro t em [0,1).
//
// Os pontos de controle estão igualmente espaçados em x (P1.x = P0.x + L/3,
// P2.x = P0.x + 2L/3), o que torna x(t) linear em t. Assim, o parâmetro t é
// obtido diretamente a partir de x, e apenas y(t) é uma curva.
//
// Fazemos P1.y = P0.y e P2.y = P3.y, de forma que a curva tem tangente
// horizontal nos topos e nos vales. Isso garante continuidade C1 entre
// segmentos consecutivos (P2, P3 e o P1 do próximo segmento são colineares).
static void BezierSegment(float x, float& t, float& y0, float& y1, float& y2, float& y3)
{
    float s = x / TERRAIN_SEGMENT_LENGTH;
    int   i = (int)floorf(s);
    t = s - i;

    y0 = ControlPointHeight(i);
    y3 = ControlPointHeight(i + 1);
    y1 = y0;
    y2 = y3;
}

// Altura y do terreno na posição (x,z) do mundo.
float TerrainHeight(float x, float z)
{
    float t, y0, y1, y2, y3;
    BezierSegment(x, t, y0, y1, y2, y3);

    // Curva de Bézier cúbica (polinômios de Bernstein)
    float s = 1.0f - t;
    return s*s*s*y0 + 3.0f*t*s*s*y1 + 3.0f*t*t*s*y2 + t*t*t*y3;
}

// Inclinação dy/dx do terreno na posição (x,z) do mundo.
float TerrainSlope(float x, float z)
{
    float t, y0, y1, y2, y3;
    BezierSegment(x, t, y0, y1, y2, y3);

    // Derivada da curva de Bézier em relação a t...
    float s = 1.0f - t;
    float dydt = 3.0f*s*s*(y1 - y0) + 6.0f*t*s*(y2 - y1) + 3.0f*t*t*(y3 - y2);

    // ... e, pela regra da cadeia, dy/dx = dy/dt * dt/dx, onde dt/dx = 1/L.
    return dydt / TERRAIN_SEGMENT_LENGTH;
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
            float y = TerrainHeight(x, z);

            model_coefficients.push_back( x );    // X
            model_coefficients.push_back( y );    // Y
            model_coefficients.push_back( z );    // Z
            model_coefficients.push_back( 1.0f ); // W

            // A normal de uma superfície y = h(x) é (-dh/dx, 1, 0), normalizada.
            float dhdx = TerrainSlope(x, z);
            float len  = sqrtf(dhdx*dhdx + 1.0f);
            normal_coefficients.push_back( -dhdx / len ); // X
            normal_coefficients.push_back(  1.0f / len ); // Y
            normal_coefficients.push_back(  0.0f );       // Z
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
            GLuint a = i*TERRAIN_VERTS_Z + j;     // (i  , j  )
            GLuint b = a + 1;                     // (i  , j+1)
            GLuint c = a + TERRAIN_VERTS_Z;       // (i+1, j  )
            GLuint d = c + 1;                     // (i+1, j+1)

            indices.push_back(a); indices.push_back(b); indices.push_back(c);
            indices.push_back(c); indices.push_back(b); indices.push_back(d);
        }
    }
    g_TerrainNumIndices = (GLsizei)indices.size();

    for (int k = 0; k < TERRAIN_NUM_CHUNKS; ++k)
    {
        TerrainChunk& chunk = g_TerrainChunks[k];

        glGenVertexArrays(1, &chunk.vertex_array_object_id);
        glBindVertexArray(chunk.vertex_array_object_id);

        // Mesmos "location" utilizados em "shader_vertex.glsl"
        chunk.VBO_model_coefficients_id   = CreateAttributeBuffer(0, 4, 4 * TERRAIN_NUM_VERTICES);
        chunk.VBO_normal_coefficients_id  = CreateAttributeBuffer(1, 4, 4 * TERRAIN_NUM_VERTICES);
        chunk.VBO_texture_coefficients_id = CreateAttributeBuffer(2, 2, 2 * TERRAIN_NUM_VERTICES);

        GLuint indices_id;
        glGenBuffers(1, &indices_id);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_id);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);

        // Índice inválido: força o preenchimento na primeira chamada de Terrain_Update()
        chunk.chunk_index = -1000000;
    }
}

// Garante que os chunks cubram o trecho do terreno ao redor da posição x do
// jogador: um chunk atrás dele e os demais à frente. Deve ser chamada a cada
// quadro, antes de Terrain_Draw().
void Terrain_Update(float player_x)
{
    int first = (int)floorf(player_x / TERRAIN_CHUNK_LENGTH) - 1;

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
