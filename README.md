# FastAppRunner

A KRunner plugin that improves application search, with a few extras:

| Trigger | Action |
|---------|--------|
| normal typing | Frequency-ranked app search |
| `/` or `/name` | Jump to a virtual desktop |
| `date` | Show current date/time (Enter copies it) |
| `>question/` | Ask an OpenAI-compatible AI (Enter copies the answer) |

## Installation

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
# reload KRunner so it picks up the plugin
kquitapp6 krunner || true
```

## AI queries (`>…/`)

Type a question wrapped like this:

```
>capital of France/
```

- Prefix: `>`
- Suffix: `/` (request fires only once you type the closing `/`)
- Result appears as a KRunner match; **Enter** copies it to the clipboard
- Default system prompt forces very short plain-text answers (under 15 lines, no markdown)

### Where to store the API key

**Option A — environment variable (recommended)**

```bash
export CEREBRAS_API_KEY="your-key-here"
```

For Plasma sessions, put it in a startup env script so KRunner sees it:

```bash
mkdir -p ~/.config/plasma-workspace/env
echo 'export CEREBRAS_API_KEY="your-key-here"' > ~/.config/plasma-workspace/env/cerebras.sh
chmod 600 ~/.config/plasma-workspace/env/cerebras.sh
# log out and back in (or reboot)
```

**Option B — config file**

Create `~/.config/krunner/fastapprunner_ai.json` (same directory as the launch-history file):

```json
{
  "apiKey": "your-key-here",
  "apiUrl": "https://api.cerebras.ai/v1/chat/completions",
  "model": "gemma-4-31b",
  "systemPrompt": "Answer very concisely in under 15 lines. Use plain text only — no markdown."
}
```

```bash
chmod 600 ~/.config/krunner/fastapprunner_ai.json
```

- `CEREBRAS_API_KEY` **overrides** `apiKey` in the file when both are set.
- `apiUrl`, `model`, and `systemPrompt` are optional; defaults match Cerebras + a concise plain-text style.

Any OpenAI-compatible chat-completions endpoint works if you change `apiUrl` / `model`.
