# Contributing

Contributions are welcome through focused issues and pull requests.

1. Create a branch from `main`.
2. Install the development extras with `python -m pip install -e '.[dev]'`.
3. Run `pytest`, `python -m compileall -q src tests`, and `git diff --check`.
4. Do not add prompts, responses, reasoning, command text, transcripts,
   credentials, private addresses, or raw hook captures to fixtures.
5. Keep Python and C++ protocol state IDs identical.

Firmware changes should be compiled on an Arduino UNO Q with the pinned core
and RouterBridge versions documented in the README.
