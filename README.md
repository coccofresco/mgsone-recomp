# Metal Gear Solid — PSX recomp italiana

Repository locale per sviluppare la ricompilazione nativa di Metal Gear Solid
PSX PAL italiano, CD1 **SLES-01508** e CD2 **SLES-11508**.

Stato: base di sviluppo, strumenti compilati, inventario degli asset 3D dei due CD
e analisi iniziale del boot e dell’eseguibile principale interno.
Non è ancora un port giocabile. Il profilo MGS e la gestione degli overlay
specifici del gioco devono essere sviluppati e verificati.

## Asset originali e presentazione Vulkan

- [Formati 3D verificati e fonti](docs/mgs-3d-formats.md): KMD/ZMD, PCX, OAR,
  archivi di scenario, luci e confini ancora da verificare.
- [Contratto di presentazione](docs/mgs-vulkan-presentation.md): mesh originali,
  istanze e posa dal codice ricompilato, proiezione float e isolamento della simulazione.
- [Evidenza sui CD italiani](docs/mgs-asset-evidence.json): 94 scenari per CD,
  parser, confronto con Rex e risorse ancora opache.

Gli strumenti di audit sono in `scripts/mgs_assets.py`; gli asset estratti e
l'esempio di geometria originale sono in `data/`, esclusi da Git.
L'integrazione Vulkan di MGS è progettata ma non ancora implementata.

## Base riutilizzata

`engine/` contiene una copia indipendente dei sorgenti del progetto locale
`/home/xander/Condivisa/crash3-recomp`: analizzatore, ricompilatore MIPS R3000A,
runtime PS1 (GPU Vulkan, GTE, SPU, CD-ROM, MDEC, BIOS HLE), test e strumenti.
Sono incluse le quattro dipendenze vendorizzate con le loro licenze, senza
richiedere submodule locali. Le modifiche presenti nella working tree di Crash
al momento della copia sono incluse per i file importati.

La provenienza, il commit di base e gli hash dei file importati sono in
[docs/engine-provenance.json](docs/engine-provenance.json).
Il progetto Crash originale non è stato modificato. Il suo profilo resta nel
motore come riferimento: non deve essere selezionato per MGS.

## Preparazione

Servono Python 3.11+, CMake 3.20+, Ninja, compilatore C++20, SDL2, Vulkan e glslc.
La configurazione può scaricare gli header Vulkan fissati dal motore se assenti.

```bash
./scripts/build_tools.sh
python3 scripts/prepare_discs.py \
  --disc1 '/home/xander/Scaricati/PSX Metal gear Solid ITA CD1 avercab0001.img' \
  --disc2 '/home/xander/Scaricati/PSX Metal gear Solid ITA CD2 avercab0001.img'
```

Lo script accetta immagini raw MODE2/2352 con la traccia dati iniziale, come
quelle presenti in Scaricati. Crea collegamenti locali in `data/discs/`, estrae
gli eseguibili attraverso l’analizzatore e produce hash, log e ricette TOML in
`data/`. Non modifica le immagini originali. `config/discs.it.json` conserva
l’identità delle immagini analizzate; non rappresenta una certificazione Redump.

Per generare una prima traduzione del solo eseguibile principale:

```bash
mkdir -p generated/disc1
build/ps1Recomp/ps1Recomp data/disc1.analysis.toml generated/disc1/recompiled_out.cpp
```

La ricetta è euristica: funzioni, chiamate indirette e abbinamenti PsyQ vanno
convalidati. La generazione non dimostra completezza né esecuzione corretta.
La build iniziale forza `PS1RECOMP_USE_STUB=ON`: il runtime non contiene MGS.

Dischi, eseguibili estratti, codice generato, build, salvataggi sotto `data/` e
riferimenti locali sotto `.local/` sono esclusi da Git.

## Prossimi passi

1. Completare l’inventario con la semantica delle risorse opache e dei caricamenti dinamici.
2. Identificare e verificare le funzioni nel main interno MGS1.EXE/MGS2.EXE e negli overlay italiani.
3. Collegare la traduzione a un profilo MGS isolato e ottenere un boot ripetibile.
4. Verificare intro, menu, gameplay, codec, filmati, audio, salvataggi e cambio CD.
5. Adattare le migliorie grafiche di Crash solo dopo aver misurato la fedeltà base.

Vedi [docs/bootstrap.md](docs/bootstrap.md) per verifiche e riferimento esterno.
Il motore mantiene la licenza GPL-3.0 originale; le dipendenze mantengono le
proprie licenze. Il gioco e i marchi appartengono ai rispettivi titolari.
