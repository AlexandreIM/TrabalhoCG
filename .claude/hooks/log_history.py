#!/usr/bin/env python3
"""Registra prompts do usuário e respostas do Claude em prompt_history.txt.

Uso (via hooks do Claude Code): log_history.py prompt | log_history.py response
Recebe o JSON do hook pela entrada padrão.
"""
import json
import os
import sys
from datetime import datetime

PROJECT_DIR = os.environ.get(
    "CLAUDE_PROJECT_DIR", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
HISTORY = os.path.join(PROJECT_DIR, "prompt_history.txt")


def last_assistant_text(transcript_path):
    """Lê o transcript (JSONL) e retorna o texto da última mensagem do assistente."""
    texts = []
    try:
        with open(transcript_path, encoding="utf-8") as f:
            for line in f:
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if entry.get("type") == "user" and isinstance(entry.get("message", {}).get("content"), str):
                    texts = []  # novo prompt: descarta respostas anteriores
                if entry.get("type") != "assistant":
                    continue
                for block in entry.get("message", {}).get("content", []):
                    if isinstance(block, dict) and block.get("type") == "text" and block.get("text", "").strip():
                        texts.append(block["text"].strip())
    except OSError:
        return ""
    return "\n\n".join(texts)


def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else ""
    data = json.load(sys.stdin)
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    if kind == "prompt":
        text = data.get("prompt", "").strip()
        header = f"\n===== USUÁRIO [{stamp}] =====\n"
    elif kind == "response":
        text = (data.get("last_assistant_message") or "").strip()
        if not text and data.get("transcript_path"):
            text = last_assistant_text(data["transcript_path"])
        header = f"\n----- CLAUDE [{stamp}] -----\n"
    else:
        return

    if text:
        with open(HISTORY, "a", encoding="utf-8") as f:
            f.write(header + text + "\n")


if __name__ == "__main__":
    main()
