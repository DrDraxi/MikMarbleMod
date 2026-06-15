# MikMarbleMod

A UE4SS C++ mod for **Marbles on Stream** that reads the final race/royale
standings and POSTs them as JSON to an HTTP endpoint. Ships with a Flask dummy
results server and an autonomous bot test runner.

## Quick start

**Just run it:** copy the contents of `dist/` into the game's
`…\Marbles on Stream\MarblesOnStream\Binaries\Win64\` folder and launch the game.

**Build from source:**

```bat
setup.bat            :: one time: pull the RE-UE4SS submodule + register the mod
build.bat --deploy   :: build the DLL and copy it into the game
```

**See the results locally:**

```bash
pip install -r server/requirements.txt
python server/server.py        :: prints standings POSTed by the mod on :5000
```

Full details — how the mod works, dependencies, the `config.txt` gotcha, and the
autonomous bot tests — are in [CLAUDE.md](CLAUDE.md).
