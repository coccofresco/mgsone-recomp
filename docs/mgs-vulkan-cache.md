# Cache originale Vulkan USA: prima integrazione

Stato verificato il 10 settembre 2026: estrazione completa di KMD/ZMD/PCX dagli
archivi STAGE.DIR dei due CD USA v1.0, cache immutabile caricata nella memoria
Vulkan e cattura della scena nativa prima del culling. È ora disponibile anche
la [sostituzione sperimentale delle mesh](mgs-original-mesh-replay.md). La
simulazione e il calcolo grafico originale continuano a funzionare.

## Costruzione e utilizzo

Dalla radice del repository:

```sh
python3 scripts/build_mgs_asset_cache.py \
  --disc1 'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).bin' \
  --disc2 'data/discs/usa/disc2/Metal Gear Solid (USA) (Disc 2) (v1.0).bin'
scripts/run_native.sh
```

Il builder verifica gli eseguibili USA, estrae gli originali e produce
`data/presentation/usa/assets.mgsc` e `manifest.json`, esclusi da Git. Lo script
di avvio carica automaticamente questa cache quando presente. Impostare
`PS1_MGS_ASSET_CACHE=off` per disabilitarla, oppure un percorso per un'altra
cache. `PS1_MGS_ASSET_VERIFY=1` verifica l'intero upload tramite lettura GPU;
`PS1_MGS_SCENE_CAPTURE=1` abilita l'adattatore di scena diagnostico. La cattura
richiede il renderer Vulkan e la cache caricata, non la modalità headless.

## Inventario e identità

| Contenuto | Risultato locale |
|---|---:|
| Scenari STAGE.DIR | 96 per CD |
| Occorrenze KMD/ZMD/PCX | 41.872 |
| Asset distinti | 19.340 |
| Definizioni KMD, inclusi modelli dentro ZMD | 859 |
| Parti di modello | 6.695 |
| Corner di quad espansi | 631.944 |
| Varianti PCX | 18.481 |
| Cache binaria | 93.918.664 byte, circa 89,57 MiB |
| Errori di parsing | 0 |

L'identità comprende SHA-256 dei byte originali e ID interno per gli elementi
ZMD. Il manifest conserva disco, scenario, contenitore, ID e offset di ogni
occorrenza. Pixel, palette e sorgenti identici condividono spazio; le varianti
con metadati diversi restano distinte. Non è una cache di tutta la logica o di
tutti i formati del gioco: OAR, luci e altri tipi non sono convertiti in risorse
grafiche da questo builder.

Nessuna famiglia LOD è ancora certificata. `extend` e `parent` vengono
conservati, senza equipararli a un livello di dettaglio. Tutte le varianti sono
residenti, ma selezionare automaticamente quella con più facce potrebbe
sostituire un oggetto con uno differente.

## Formato e risorse Vulkan

`MGSAC001` ha intestazione con lunghezza e CRC32, directory di sette sezioni
allineate a 16 byte e record di dimensione fissa:

- `ASST`: identità e intervalli delle risorse, 64 byte.
- `MODL`: parti, parent, extend e intervallo corner, 32 byte.
- `VRTX`: posizione originale, seam, normale, UV, texture ID e riferimenti
  modello/parent, 48 byte per corner.
- `TEXR`: dimensioni, intervalli pixel/palette e flag, 32 byte.
- `TEXI`: indici palette a un byte, senza conversione in RGBA.
- `PLTE`: parole RGB555/STP conservate in elementi uint32.
- `SRCE`: byte originali per controlli e stream indicizzati.

Il seam indica un vertice sorgente del parent, non un peso di skinning.
Il lettore host controlla CRC, sezioni, intervalli e relazioni. L'upload usa
il dispositivo, la coda e il pool del renderer ereditato, un buffer device-local
con uso storage/transfer e uno staging temporaneo. Il buffer resta residente
attraverso frame, ricreazione swapchain e caricamento savestate. Il dispositivo
deve supportare una storage-buffer range almeno pari alla cache; non è ancora
implementata la suddivisione su più buffer. Lo shader MGS opzionale legge direttamente questi record; l'adattatore di
scena ne certifica la corrispondenza con i pacchetti da sostituire.

## Cattura della scena USA

L'override nativo è a `DG_BoundChanl`, `0x8001919C` USA v1.0, verificato contro
istruzioni e strutture del titolo. Acquisisce camera, gruppo, visibilità
semantica e matrici world dopo la composizione nativa e prima dei test geometrici.
Il recipe controlla l'eseguibile USA e l'istruzione sovrascritta, poi esegue anche
l'operazione originale. Lo snapshot possiede i dati e non espone RAM guest al
renderer. Un caricamento savestate o una cattura non valida invalida lo snapshot.

Il collegamento alla cache richiede l'intera definizione geometrica originale,
ricostruendo rilocazione dei puntatori e riscrittura dei seam. Non bastano ID a
16 bit, indirizzi RAM o conteggi. Corrispondenze multiple sono rifiutate.
La ricerca è ripetuta a ogni cattura: copre il ripristino senza osservare di nuovo
il loader, ma è ancora diagnostica e non ottimizzata per l'uso finale.

Sul checkpoint locale del molo sono stati osservati 27 oggetti, 22 semanticamente
visibili, 295 parti e 19 oggetti associati con certezza. Gli otto restanti sono
**non certificati**, senza dedurre se la causa sia deformazione, variante o
modifica runtime. La modalità di sola cattura non sopprime pacchetti. La nuova modalità mesh
verifica anche posa e provenienza dei pacchetti prima di sostituirli.

## Verifiche riproducibili

```sh
python3 -m unittest discover -s scripts/tests
PS1_TEST_MGS_ASSET_CACHE=data/presentation/usa/assets.mgsc \
  build/ps1Test/ps1Runtime_tests \
  --gtest_filter='MgsAssetCacheTest.*:*Mgs*:*SaveState*'
```

Risultato: 25 test Python e 20 test C++ passati, inclusi parsing, CRC/range,
matching geometrico, cattura prima del culling, invarianza RAM/registri e
invalidazione degli snapshot. La verifica locale Vulkan ha caricato slot 0
senza salvarlo e proseguito per 600 VBlank: un solo upload di 93.918.664 byte,
readback interamente identico, 398 catture di scena. Log e metriche locali sono
in `.local/` e `data/native/asset-cache/`; non includiamo gli asset originali in Git.
Il test di invarianza è sintetico; non costituisce ancora un confronto completo
di gameplay con una presentazione sostitutiva.

Il passo successivo ha implementato trasformazioni extend/seam, cattura per
buffer e disegno Vulkan dei pacchetti certificati. Restano materiali autonomi,
deformazioni, ampliamento della geometria oltre i pacchetti originali e LOD. Il [contratto di presentazione](mgs-vulkan-presentation.md) descrive
questi passaggi e i criteri di verifica.
