# MGS italiano: formati 3D e risorse originali

Ricognizione del 10 settembre 2026, sui CD identificati in
[`config/discs.it.json`](../config/discs.it.json). Obiettivo: alimentare la
presentazione Vulkan con geometria originale e stato semantico della simulazione
ricompilata, prima delle perdite dovute a proiezione, clipping e pacchetti PS1.

Le strutture di FoxdieTeam descrivono **Integral**. I layout degli asset riportati
qui sono stati anche controllati sui dischi italiani; indirizzi di funzioni,
layout degli oggetti in RAM e comportamento dei singoli attori richiedono ancora
conferma nell'eseguibile italiano. La decodifica offline non prova il rendering.

## Fonti e criterio di confronto

Le revisioni esatte sono in [mgs-references.json](mgs-references.json); i checkout
sono in `.local/`. I sorgenti esterni sono stati consultati, non importati.

| Progetto | Evidenza utilizzata | Limite |
|---|---|---|
| [FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing) | `source/include/fmt_*.h`, `libfs/cdstage.c`, `libdg/*`, `game/motion.c`, `game/map.c`: loader e consumatori del gioco | Integral, non mappa indirizzi PAL ITA |
| [Jayveer/Rex](https://github.com/Jayveer/Rex) | `mgs/archive/{stage,dar}`, `mgs/config/cnf`: struttura degli archivi; confronto con estrazione effettiva | Nomi stage lunghi 8 caratteri problematici nella revisione consultata |
| [virusek20/mint](https://github.com/virusek20/mint) | `Kmd`, `Pcx`, `Stg`, `Dar/Psx`: lettura/scrittura, collegamenti vertici parent, metadati texture | Anche PC; le riparazioni di mesh e il round-trip GLTF non sono regole di rendering originali |
| [Jayveer/MGS-KMD-Noesis](https://github.com/Jayveer/MGS-KMD-Noesis) | `kmd.h`, `mesh.h`, `bone.h`, `motion.h`: seconda interpretazione dei modelli e OAR | Importatore di visualizzazione, con approssimazioni esplicite nell'animazione |

I simboli del motore e i punti di aggancio sono descritti nel
[contratto di presentazione](mgs-vulkan-presentation.md).

## Percorso dal disco agli asset

```text
IMG raw MODE2/2352
  → settori dati Form1 da 2048 byte → ISO9660
  → /MGS/STAGE.DIR → directory di scenari → configurazioni DATACNF
  → risorse dirette e archivi DAR PSX
  → KMD / ZMD, PCX, OAR, LIT, HZM e altre risorse
```

`STAGE.DIR` inizia con un `u32` little-endian che misura **in byte** la tabella;
ogni record è `char name[8]` + `u32 sector_offset`. Gli offset sono relativi
all'archivio in settori da 2048 byte. Il nome può occupare tutti gli otto byte.

Il blocco di scenario usa `DATACNF`: versione `u8`, padding `u8`, dimensione
stage `u16` in settori, poi record `{u16 id, u8 mode, u8 ext, u32 size}`.
Il lettore implementato supporta versione 1/padding 0, verificati nel corpus.
Il payload comincia al settore successivo; la modalità zero termina i record.

- `n` e `r`: dati con dimensione esplicita, prossimo blocco allineato a 2048.
- `c`: il campo size è un **offset cumulativo**, non la lunghezza. La lunghezza
  è la differenza con il record successivo; estensione `0xff` chiude il gruppo.
- `s`: payload sonori gestiti dal caricatore; vengono inventariati, non convertiti.
- DAR PSX: sequenza di `{u16 hash, u16 estensione, u32 lunghezza, payload}`.
  Non usare il formato DAR della versione PC, che ha un'organizzazione diversa.

Le estensioni sono codici ASCII: `k` KMD, `z` ZMD, `p` PCX, `o` OAR, `l` LIT,
`h` HZM. I nomi degli asset sono hash a 16 bit, non identificatori globalmente
univoci. Il nostro inventario mantiene scenario, indice di configurazione,
modalità, offset e SHA-256; conserva anche occorrenze ripetute.

Fonti: [DATACNF](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libfs/datacnf.h),
[caricatore CD](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libfs/cdstage.c),
[lettore Rex](https://github.com/Jayveer/Rex/blob/master/mgs/config/cnf/cnf.cpp).

## KMD: geometria prima della proiezione

Non c'è una magic obbligatoria. Header di **32 byte**: due contatori `i32` e due
vettori di bounding box `3 × i32`. Seguono `n_x_models` descrittori di **88 byte**.
Il primo contatore è `n_models` nel gioco, il secondo `n_x_models`: etichettarli
semplicemente "numero ossa/mesh", come negli importatori, perde il ruolo degli
oggetti aggiuntivi collegati tramite `extend`.

| Offset nel descrittore | Tipo | Significato |
|---|---|---|
| `0x00`, `0x04` | `u32`, `i32` | flag, numero facce |
| `0x08`, `0x14` | `3 × i32` ciascuno | due estremi del volume locale, conservati nell'ordine originale |
| `0x20` | `3 × i32` | traslazione locale |
| `0x2c`, `0x30` | `i32` | parent, extend; `-1` significa assente |
| `0x34`, `0x38`, `0x3c` | `i32`, `u32`, `u32` | numero vertici, offset vertici, offset indici facce |
| `0x40`, `0x44`, `0x48` | `i32`, `u32`, `u32` | numero normali, offset normali, offset indici normali |
| `0x4c`, `0x50`, `0x54` | `u32` ciascuno | offset UV, offset texture ID per faccia, padding |

Gli offset KMD su disco sono relativi alla base del file, non puntatori host.
I vertici sono `{i16 x,y,z,pad}`; le normali hanno lo stesso contenitore, con
scala 4096 per la componente unitaria. Ogni faccia conserva quattro indici
vertice `u8`, quattro indici normale `u8`, quattro coppie UV `u8,u8` e un texture
ID `u16`. Il bit alto dell'indice normale va conservato: l'indice base è `& 0x7f`,
ma il bit trasporta informazione usata dal percorso di shading.

Il quarto campo del vertice **non è un peso di skinning**: `-1` indica un vertice
proprio; altrimenti identifica un vertice della mesh parent con cui mantenere la
giunzione. `DG_LoadInitKmd` riloca i puntatori e `LinkModelToParent` modifica gli
indici e questo campo per riferirsi ai pacchetti del parent. Il parser offline
opera sui byte originali; un parser per la RAM già inizializzata sarebbe diverso.
Non si devono applicare automaticamente i metodi di "repair" di MINT agli asset.

`extend` collega geometria aggiuntiva: deve essere percorsa anche quando non è
contata negli oggetti primari. I flag confermati dalle definizioni includono
`0x2` trasparenza, `0x4` unlit, `0x400` doppia faccia, `0x10000` shading indiretto.
Non sono una descrizione completa di tutti i bit né di tutte le modifiche runtime.

**Ordine dei corner:** `DG_WriteObjPacketUV` converte l'ordine sorgente in
`[0,1,3,2]` nei pacchetti PS1. Noesis emette invece i triangoli `[0,2,1]` e
`[0,3,2]` nel proprio sistema. Winding, cambio di assi e diagonale devono essere
scelte esplicite: su quad non planari le triangolazioni possono differire.
Il formato neutro esportato conserva le quattro facce-corner originali e non
sceglie silenziosamente una triangolazione. I corner finali uguali sono contati,
ma il loro conteggio non costituisce una certificazione della topologia renderizzata.

Fonti: [layout KMD](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/include/fmt_kmd.h),
[rilocazione e giunzioni](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/loader.c),
[oggetti ed extend](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/obj.c),
[UV e corner](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/opack.c),
[mesh Noesis](https://github.com/Jayveer/MGS-KMD-Noesis/blob/master/mesh.h).

## ZMD: modelli con dati condivisi

Magic `KMDa`, poi numero KMD, offset pool vertici e lunghezza pool, tutti `u32`:
header 16 byte. Seguono record `{u32 id, DG_DEF, DG_MDL[]}`. Il pool è a
`16 + vert_offset`; gli offset di geometria sono relativi a quel pool, non al
singolo header KMD. Il loader registra i modelli interni nella cache con tipo `k`.

Nei CD italiani: `s11a` contiene 7 modelli, `s11c` 11, `s14e` 6, `s15a` 3.
Il parser verifica anche questi **27 modelli**, oltre ai KMD indipendenti.
Fonte: `DG_LoadInitKmdar` nello stesso
[loader](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/loader.c).

## PCX: texture, materiali e palette

Sono PCX con metadati MGS nei byte `74..87`: stamp 12345, flag, coordinate VRAM
della texture e della CLUT, numero colori. Il corpus contiene PCX 1 bit × 4 piani
(16 colori) e 8 bit × 1 piano (256 colori). Il decoder implementato segue la RLE
del gioco, inclusa la gestione di `0xc0` come letterale; verifica il flusso,
ricostruisce gli indici e legge la palette.

Le UV KMD sono normalizzate rispetto alla texture mediante divisione per 256;
il gioco le scala alla dimensione effettiva e aggiunge l'offset nella pagina VRAM.
In Vulkan si possono mantenere UV frazionarie e texture isolate, evitando di
ricavare le UV dai byte già arrotondati del pacchetto. Devono però restare
associati identità e generazione del materiale effettivamente selezionato.

La palette RGB non è solo colore: `LoadPalette` usa i bit bassi dei componenti
per lo STP PS1. Zero è trasparente; il bit STP influenza il blend, non equivale a
un alpha generico. I flag PCX contengono profondità texture e ABR. CLUT animate,
texture cambiate, termografia, maschere e framebuffer feedback richiedono lo
stato runtime; non basta caricare un PNG una volta. Anche un percorso true-color
deve distinguere i bit che codificano semantica dai componenti cromatici.

Fonti: [metadati texture](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/include/fmt_tex.h),
[cache texture](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/text.c),
[palette e caricamento](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/loader.c),
[termografia](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/bound.c).

## OAR: archivio animazioni e posa effettiva

Su disco magic `OARa`, numero joint, numero motion e campo dimensione in word;
header 16 byte. La tabella contiene `(n_joint + 2) × n_motion` word `u16`:
per motion, numero frame e offset degli stream di movimento/rotazione. Gli
offset sono in word rispetto all'archivio che segue la tabella.

Il loader sostituisce il primo e quarto campo con puntatori runtime. La tabella
è verificata dal nostro strumento, **i bitstream animati non sono ancora decodificati**.
Per la presentazione in gioco la prima fonte della posa sarà lo stato prodotto
dal codice ricompilato: blending, rots, movs, adjust, waist rotation e root,
non la riproduzione autonoma di un OAR scelto soltanto per numero di ossa.

Noesis assume 30 fps e contiene incertezze esplicite sui keyframe di traslazione;
non è un oracolo del timing PAL. `game/motion.c` gestisce bitstream, maschere,
interpolazione e wrapping degli angoli a 4096 unità per giro. Una futura posa
float deve riprodurre quella composizione e interpolare solo la presentazione.

Fonti: [layout motion](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/include/fmt_mot.h),
[animazione del gioco](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/game/motion.c),
[composizione matrici](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/screen.c),
[importatore OAR](https://github.com/Jayveer/MGS-KMD-Noesis/blob/master/motion.h).

## Scenari, luci, collisioni ed effetti

- KMD/ZMD sono anche geometria ambientale: non sono riservati ai personaggi.
  `game/map.c` lega hazard, geometria e luci per la mappa.
- LIT ordinario: conteggio `u32` seguito da record 16 byte con posizione SVECTOR,
  raggio interno/esterno `u16` e colore CVECTOR. In `s03b`, otto risorse con
  estensione `l` non rispettano questo layout: restano opache e segnalate.
  Questo non prova che i dati siano corrotti; l'estensione non certifica il tipo.
- HZM/HZD: collisioni, segmenti verticali, pavimenti, volumi trigger, percorsi e
  camere. Sono dati della simulazione, **non un sostituto della mesh visibile**.
- `DG_PRIM` copre geometria procedurale, sprite e altri effetti; IMG/SGT e altre
  risorse richiedono analisi dei rispettivi consumatori. Un renderer KMD-only
  non coprirebbe l'intera immagine né tutte le cutscene.

Fonti: [mappa](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/game/map.c),
[luci](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/include/fmt_lit.h),
[hazard](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/include/fmt_hzd.h),
[primitive](https://github.com/FoxdieTeam/mgs_reversing/blob/master/source/libdg/prim.c).

## Evidenza e riproduzione

| Risorsa | Occorrenze per CD | Contenuti distinti per CD | Verifica attuale |
|---|---:|---:|---|
| Scenari | 94 | — | Configurazioni e range dei payload |
| KMD | 1829 | 825 | Stream geometrici, indici, parent/seam, extend |
| ZMD | 4 | 4 | 27 KMD interni e pool condivisi |
| PCX | 18486 | 17962 | RLE, indici e palette; nessun indice fuori palette |
| OAR | 166 | 89 | Header e tabelle, non bitstream animati |
| LIT | 323 | 117 | 315 compatibili con DG_LITS; 8 risorse opache in s03b |

Il corpus contiene 21.629 risorse per CD. Fra i due CD, 21.516 occorrenze hanno
uguale identità di archivio e contenuto; 113 differiscono, tutte di tipo `g`
(GCX). KMD, ZMD, PCX e OAR quindi coincidono in questo confronto. La risoluzione
statica trova 10 coppie scenario/texture ID non disponibili nello stesso stage:
cache residenti e selezioni runtime devono ancora essere ricostruite; non sono
state sostituite con texture arbitrarie.

I risultati numerici, gli hash, il confronto fra CD e le lacune sono in
[mgs-asset-evidence.json](mgs-asset-evidence.json). Gli inventari completi e i
payload sono locali sotto `data/assets/`, esclusi da Git.

```bash
python3 -m unittest discover -s scripts/tests -v
python3 scripts/mgs_assets.py audit --disc data/discs/disc1.bin --out data/assets/disc1
python3 scripts/mgs_assets.py audit --disc data/discs/disc2.bin --out data/assets/disc2
# Esempio reale di modello con 16 oggetti primari, senza attribuzione del nome originale.
mkdir -p data/examples
python3 scripts/mgs_assets.py extract --inventory data/assets/disc1/inventory.json \
  --stage s00a --id 7693 --type k --config-index 6 --out data/examples/s00a-7693.kmd
python3 scripts/mgs_assets.py kmd data/examples/s00a-7693.kmd \
  --out data/examples/s00a-7693.geometry.json
```

L'audit restituisce 1 se una risorsa non passa il parser selezionato, ma salva
l'inventario completo: attualmente le otto risorse `l` segnalate impediscono un
esito interamente verde. Non implica che tutte le altre semantiche siano note:
OAR è verificato solo fino alla tabella, LIT ordinario solo nelle dimensioni,
e HZM/IMG/SGT e altre estensioni sono soltanto inventariate.

La lettura ISO identifica anche `/MGS/MGS1.EXE` e `/MGS/MGS2.EXE`: sono identici
nei due CD, 645120 byte, entry `0x8009a35c`. L'analisi euristica trova 2262
funzioni e 116 abbinamenti PsyQ. Il lavoro precedente riguardava invece il boot
`SLES_*`; gli agganci grafici vanno cercati nell'eseguibile interno e negli overlay.

### Confronto indipendente con Rex

La revisione fissata è stata compilata su Linux senza modificare i sorgenti,
aggiungendo gli include standard mancanti nella riga di compilazione:

```bash
c++ -std=c++17 -include cstdint -include cstring -include iterator \
  .local/Rex/main.cpp .local/Rex/interface/cli.cpp \
  .local/Rex/mgs/archive/stage/stage.cpp .local/Rex/mgs/archive/dar/dar.cpp \
  .local/Rex/mgs/config/cnf/cnf.cpp -o .local/rex
mkdir -p data/rex-disc1
.local/rex data/assets/disc1/STAGE.DIR data/rex-disc1
python3 scripts/compare_rex.py data/assets/disc1/STAGE.DIR \
  data/rex-disc1/stage --out .local/rex-comparison.json
```

Tutti i 3189 file prodotti da Rex coincidono byte per byte con i payload letti
indipendentemente dal nostro parser. Rex lascia vuoti `init_tux` e `selectvr`,
nomi di otto caratteri: mancano rispettivamente 5 e 10 payload, che il nostro
lettore recupera. Il confronto restituisce perciò 1 e registra lo scarto, senza
presentarlo come un accordo completo di tutti i 3204 payload. Il confronto
riguarda le risorse al primo livello, inclusi i DAR, non la resa di Noesis/MINT.
