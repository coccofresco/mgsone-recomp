# Metal Gear Solid — PSX recomp

Repository locale per sviluppare la ricompilazione nativa di Metal Gear Solid
PSX. Riferimento attuale: **USA v1.0**, CD1 **SLUS-00594** e CD2 **SLUS-00776**.
Le immagini italiane e il relativo audit restano disponibili come confronto.

Stato: **avvio nativo USA verificato**, dai loghi Konami all’inizio della
sequenza subacquea, con presentazione Vulkan e temporizzazione NTSC.
È verificato anche il **gameplay nel molo iniziale**, con movimento di Snake
e apertura del Codec. Sono ricompilati `title`, `opening`, `d00a`, `s00a` e
`s01a`; il passaggio all’eliporto e i salvataggi dal gioco restano da validare.
**F5/F9 ora salvano e ripristinano anche dopo il riavvio del runtime.**
Non è ancora un port completo.

Il profilo USA abilita di base la proiezione 16:9. SwanStation strumentato è
il donor/oracolo e usa CPU al 165%; il codice nativo gira alla velocità
host, mantenendo i divisori originali 30/20 fps. Non è ancora implementato
un modello di budget CPU nativo equivalente esattamente al 165%.

- [Stato, verifiche e limiti del runtime](docs/mgs-runtime-progress.md)
- [Controlli e memory card persistenti](docs/mgs-input-memory-card.md)
- [Savestate nativi: tasti, formato e verifiche](docs/mgs-savestates.md)
- [Aggiornamenti PSXRecomp](docs/psxrecomp-upstream-2026-09-10.md)
- [Valutazione RecompOne](docs/recompone-review-2026-09-10.md)
- [Valutazione OpenGTPS1: input e memory card](docs/opengtps1-review-2026-09-10.md)

## Asset originali e presentazione Vulkan

- [Formati 3D verificati e fonti](docs/mgs-3d-formats.md): KMD/ZMD, PCX, OAR,
  archivi di scenario, luci e confini ancora da verificare.
- [Contratto di presentazione](docs/mgs-vulkan-presentation.md): mesh originali,
  istanze e posa dal codice ricompilato, proiezione float e isolamento della simulazione.
- [Evidenza sui CD italiani](docs/mgs-asset-evidence.json): 94 scenari per CD,
  parser, confronto con Rex e risorse ancora opache.

Gli strumenti di audit sono in `scripts/mgs_assets.py`; gli asset estratti e
l'esempio di geometria originale sono in `data/`, esclusi da Git.
La [cache Vulkan USA](docs/mgs-vulkan-cache.md) contiene circa 90 MiB residenti
con geometrie KMD/ZMD e texture PCX originali dei due CD. La
[presentazione dalla cache](docs/mgs-original-mesh-replay.md) è disponibile con
`scripts/run_native.sh`: la [presentazione UHD](docs/mgs-uhd-presentation.md)
è ora predefinita, a 3840×2160 e 16:9 fisso. Tutte le 27 istanze del molo,
deformazioni, suddivisioni, texture e quad degli effetti sono supportati.
Vulkan legge gli originali e proietta in float; simulazione e framebuffer PS1
continuano sotto al cofano. HUD e Codec sono composti come grafica 2D runtime.
Le [pose articolate](docs/mgs-modern-pose.md) sono ora ricostruite dagli operandi
originali in virgola mobile, inclusi gli agganci fra modelli; le matrici PS1
restano l'oracolo separato per verificare i pacchetti.
La [scena sorgente unificata](docs/mgs-unified-source-scene.md) prepara la
geometria anche senza pacchetti PS1; modelli ed effetti riconosciuti condividono
proiezione, profondità e binding della cache. Il recupero delle facce interamente
assenti è attivo per i materiali opachi con colori persistenti verificabili.
Il percorso resta sperimentale: altre scene, culling completamente indipendente, famiglie LOD,
interpolazione e prestazioni richiedono ulteriori verifiche e sviluppo.

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

Per il target attuale USA v1.0:

```bash
python3 scripts/prepare_usa_discs.py
./scripts/build_native.sh 'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).cue'
./scripts/run_native.sh
```

La build copre l’avvio iniziale, non il gioco completo. Gli archivi,
i CD estratti e il codice generato rimangono esclusi da Git.

Preparazione precedente dei CD italiani:

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
2. Verificare il passaggio all’eliporto e il primo salvataggio/caricamento dal gioco.
3. Completare gli overlay USA v1.0 degli scenari successivi.
4. Estendere le verifiche a combattimento, filmati, audio e cambio CD.
5. Adattare le migliorie grafiche di Crash solo dopo aver misurato la fedeltà base.

Vedi [docs/bootstrap.md](docs/bootstrap.md) per verifiche e riferimento esterno.
Il motore mantiene la licenza GPL-3.0 originale; le dipendenze mantengono le
proprie licenze. Il gioco e i marchi appartengono ai rispettivi titolari.
