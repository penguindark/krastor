# COLD_START — dalla clone all'esecuzione

Guida punto per punto per avere **Krastor** funzionante partendo dai soli
sorgenti git, su Windows 10/11 x64. (Linux: `scripts/build.sh` + libcurl di
sistema; i binari sherpa-onnx prebuilt sono solo win-x64, quindi il percorso
consigliato è Windows.)

Il repo contiene **tutto il codice sorgente e le risorse minime** (font, icone
SVG, shader, atlas emoji, dati linguaggi). Non contiene: binari prebuilt
(sherpa-onnx), pesi dei modelli AI, `config.json` — si recuperano ai punti 3-5.

---

## 1. Prerequisiti (una tantum)

| Cosa | Versione testata | Note |
|---|---|---|
| Git | 2.53+ | serve per i submodule |
| CMake | 4.4.3 | `winget install Kitware.CMake` |
| Compilatore C++20 (uno dei due) | MinGW-w64 GCC 16.2 in `C:\mingw64` **oppure** Visual Studio 2026/2022 con workload "Desktop C++" | entrambi supportati; la build script li seleziona con `-Toolchain` |
| Python 3 | 3.13 (basta anche una 3.11+) | solo per i due script di download, usano solo libreria standard |
| GPU con OpenGL 3.3 core | — | richiesto dal GUI (GLFW + Nuklear) |

Verifica rapida:

```powershell
git --version; cmake --version; python --version
# e uno dei due:
g++ --version          # MinGW
# (MSVC viene trovato da CMake via vswhere, non serve in PATH)
```

## 2. Clonare il repository

```powershell
git clone --recursive <URL-del-repo> krastor
cd krastor
```

`--recursive` porta dentro i submodule: **`libs/lunasvg`** (rasterizzatore
SVG, tag v3.5.0; plutovg è già vendorato dentro, nessun sotto-submodule),
**`libs/rnnoise`** (noise suppression del micro) e **`libs/speexdsp`**
(eco cancellazione + resampler). Se cloni senza `--recursive` nessun
problema: `scripts/build.ps1` esegue automaticamente
`git submodule update --init --recursive` quando ne manca uno.

Tutto il resto delle librerie è già nel repo (header singoli o sorgenti:
GLFW, Nuklear, glad, nlohmann/json, miniaudio, stb).

## 3. Scaricare i binari esterni (sherpa-onnx + pesi RNNoise)

Sono git-ignored (regola globale `*.dll`/`*.lib` + `libs/rnnoise-data/`).
Circa 40 MB:

```powershell
python tools\download_deps.py
```

Porta in `libs/sherpa-onnx/lib/` i 4 file `sherpa-onnx-c-api.dll/.lib`,
`onnxruntime.dll`, `onnxruntime_providers_shared.dll` (release pinned
1.13.8 — **non** aggiornare a 2.x, ignora `data_dir` e rompe le voci Piper)
e in `libs/rnnoise-data/src/` le tabelle C dei pesi del denoiser RNNoise
(il repo git di RNNoise non le contiene; tarball verificato via sha256).
La build CMake copierà poi le DLL accanto a `bin\krastor.exe` da sole.

## 4. Scaricare i modelli AI (voce) — `models/`

La cartella `models/` è git-ignored. Senza modelli il programma **parte
comunque** (chat LLM funzionante, TTS/STT silenziosi); per la voce:

```powershell
# tutto (~2.5 GB: STT it/en, TTS it/en, VAD, frontend espeak-ng)
python tools\download_models.py

# oppure solo il set minimo italiano (TTS + STT hands-free, ~800 MB):
python tools\download_models.py --only espeak-ng-data
python tools\download_models.py --only tts-piper-it-it-paola
python tools\download_models.py --only vad-silero
python tools\download_models.py --only stt-parakeet-multilingual
```

Nomi disponibili: `stt-vosk-it`, `stt-vosk-it-large`, `stt-vosk-en`,
`stt-parakeet-multilingual`, `stt-whisper-small`, `vad-silero`,
`espeak-ng-data`, `tts-piper-it-it-paola`, `tts-piper-en-us-lessac`.
Ogni modello finisce in `models\<nome>\` con il proprio README (fonte,
licenza). `espeak-ng-data` serve sempre se si usa una voce Piper.

## 5. Configurare LLM e config

Due file JSON (entrambi cercati **accanto a `krastor.exe`**, poi nella
working dir):

1. **`llm_models.json`** — registro dei modelli LLM OpenAI-compatibili
   (URL, chiave, contesto). Nel repo c'è una copia di esempio con endpoint
   LAN locali: **modificalo** con il tuo server (LM Studio, llama.cpp,
   ollama, Crucible…) o un provider cloud. La sezione Settings della GUI
   poi deve solo scegliere il modello da questa lista.
2. **`config.json`** — config runtime (git-ignored). Dopo la build:

   ```powershell
   copy config.example.json bin\config.json
   ```

   Campi principali: `llm.model_ref` (voce di `llm_models.json` selezionata;
   in alternativa `llm.base_url`/`api_key`/`model` per un endpoint one-off,
   con fallback env `KRASTOR_API_KEY` → `OPENAI_API_KEY`),
   `tts.default_voice` (`it-it-paola`), `stt.input_device` (-1 = default),
   `gui.*` (font, effetto sfondo, dimensione finestra), `agent.*`,
   `tools.enabled` (master switch dei tool esterni, vedi `tools_plan.md`),
    `sessions.*` (`dir` vuoto = `sessions\` accanto a `bin\`, `autosave`,
    `continue_last` — vedi `sessions_plan.md`).
   La GUI salva qui le proprie modifiche (Settings, zoom font, resize).

## 6. Compilare

```powershell
# MinGW:
.\scripts\build.ps1 -Toolchain mingw -Config Release
# oppure MSVC 2026 (default del script):
.\scripts\build.ps1 -Toolchain msvc2026 -Config Release
```

Lo script mappa sul preset CMake (`mingw-release`, `msvc-2026-release`…),
inizializza i submodule se mancano, configura e builda. Output:
`bin\krastor.exe` (+ DLL sherpa copiate lì accanto) **più i tool eseguiti
dell'agente** (`tools\filetool.exe`, `tools\webfetch.exe`,
`tools\websearch.exe`, accanto ai loro manifest `tool_*.jsonc`).
Usa `-Config Debug` per gli sviluppatori (entrambe le toolchain devono
compilare pulite: è una regola del progetto).

### 6b. Virtualenv Python dell'agente (opzionale)

Il tool `python` esegue codice in un virtualenv dedicato, `tools\python\venv`
(git-ignored). Senza, il tool resta disabilitato (lo dice il log all'avvio);
per attivarlo:

```powershell
powershell -ExecutionPolicy Bypass -File tools\python\setup.ps1
```

## 7. Eseguire

**Lancia sempre dalla root del repo** (i percorsi `data/icons`, `data/fonts`,
`data/shaders` sono relativi alla working dir; dalla root funzionano):

```powershell
cd C:\Dev\krastor
.\bin\krastor.exe --gui
```

Finestra con chat stile terminale, sfondo shader, icone SVG (barra in alto:
sessione corrente a tendina + tre pulsanti solo icona: hands-free, parlata,
settings; cluster in basso a destra: send, allega immagine, ascolta, prova
voce, pulisci schermo, help, stop). Enter invia, Ctrl+V incolla testo o
allega un'immagine, drag & drop di file nel box.
Le chat sono **salvate automaticamente** in `sessions\` (una cartella per
sessione): `Ctrl+O` apre il gestore sessioni, `Ctrl+Shift+O` cicla le
recenti, `/compact` riassume la sessione con il modello attivo, `/clean`
(pulsante gomma) pulisce lo schermo senza toccare la sessione, `F1` o
`/help` apre l'elenco di comandi e scorciatoie.

Senza argomenti parte la CLI (stessa chat, testuale): comandi `/sessions`,
`/session new|open|delete|rename`, `/compact`; flag `--session <id>`,
`--new-session`, `--list-sessions`, `--delete-session <id>`, `--compact`.

## 8. Verifiche rapide

```powershell
# atlas delle icone SVG senza GUI: rasterizza le 11 icone, controlla
# l'coverage e scrive icon_atlas_test.png da ispezionare
.\bin\krastor.exe --icon-test
.\bin\krastor.exe --fx-test
# sistema dei tool esterni: gira i 3 tool senza LLM (scrittura nel sandbox,
# deny floor, webfetch, python) e stampa PASS/FAIL per riga
.\bin\krastor.exe --tool-test
# sistema delle sessioni: round-trip cartella/alleati/tool call, tolleranza
# ai crash, archivio del compact, delete — senza LLM
.\bin\krastor.exe --session-test
```

- Icone mancanti/emoji al posto delle SVG → sei partito da una cartella che
  non contiene `data/icons` (lancia dalla root) oppure `--icon-test` fallisce.
- `--fx-test` FAIL → i pesi RNNoise mancano o sono corrotti: riesegui
  `python tools\download_deps.py`.
- Voce muta → mancano `models\tts-piper-*` + `models\espeak-ng-data` (punto 4).
- "LLM error" → `llm_models.json`/`config.json` puntano a un endpoint non
  raggiungibile (punto 5).
- Errore finestra/GL → GPU o driver senza OpenGL 3.3 core.

## 9. Risoluzione problemi

| Sintomo | Causa / fix |
|---|---|
| CMake: "lunasvg submodule missing" | `git submodule update --init --recursive` |
| CMake: "RNNoise model data missing" | `python tools\download_deps.py` (pesi RNNoise in `libs\rnnoise-data\`) |
| Link error su `sherpa-onnx-c-api.lib` | non hai eseguito il punto 3 |
| `onnxruntime.dll` mancante accanto all'exe | rebuild (la copy post-build non è girata) o copia manuale da `libs\sherpa-onnx\lib\` |
| Config non salvata | `bin\config.json` è in sola lettura / cartella protetta; sposta tutto fuori da `Program Files` |
| Errore runtime non spiegato | guarda `bin\krastor_error.log` (accanto all'exe) |

---

Riassunto comandi, in ordine:

```powershell
git clone --recursive <URL> krastor; cd krastor
python tools\download_deps.py
python tools\download_models.py --only espeak-ng-data
python tools\download_models.py --only tts-piper-it-it-paola
python tools\download_models.py --only vad-silero
python tools\download_models.py --only stt-parakeet-multilingual
# (modifica llm_models.json con il tuo endpoint LLM)
.\scripts\build.ps1 -Toolchain mingw -Config Release
copy config.example.json bin\config.json
.\bin\krastor.exe --gui
```
