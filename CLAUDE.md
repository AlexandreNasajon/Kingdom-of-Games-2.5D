# jCodeMunch Instructions for KOG Editor

Use jcodemunch-mcp for all code lookups. Never read full files when MCP is available.

1. Call `list_repos` first — if the project is not indexed, call `index_folder` with the current directory.
2. Use `search_symbols` / `get_symbol` to find and retrieve code by symbol name.
3. Use `get_repo_outline` or `get_file_outline` to explore structure.
4. Fall back to direct file reads only when editing or when MCP is unavailable.
