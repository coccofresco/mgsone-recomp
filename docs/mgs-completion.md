# Verifiche di completabilità senza partita continua

La priorità attuale sono i [passaggi obbligati della campagna](mgs-required-paths.md):
la fuga con ketchup passa ora fino al recupero dell'equipaggiamento e all'uscita
dall'area. Corretto il crash della recomp dopo il primo scontro con Meryl:
il generatore ignorava una callback valida dopo un ritorno con delay slot utile.
Il blocco dopo l'Hind dipendeva invece dalla mappa sbagliata nella fixture;
con l'ingresso corretto passano anche il filmato senza salto e il ritorno ai
comandi. Le prove includono SwanStation e replay nativi esatti; fixture e
timeout storici restano espliciti. Gli aggiornamenti non certificano un'intera
partita né i combattimenti completi. Dettagli e identità sono nel
[rapporto dei passaggi obbligati](mgs-required-paths-next-evidence.json).

L'ultima [verifica del bus e dei controller](mgs-mmio-verification.md) corregge
accessi mancanti ai registri, esecuzione di comandi DMA intermedi e incoerenza
degli ingressi durante il TAS. Passano **1.316 test runtime**, con 31 esclusi,
sia normalmente sia con ASan/UBSan. Passano inoltre Codec con donor, cambio
disco, caricamento CD2, filmato completo e replay storico. Le prove e i limiti
sono nel [rapporto della build `9420f2b9…`](mgs-mmio-evidence.json).
Non vengono aggiunti segmenti di campagna o risultati visivi non verificati.

L'aggiornamento sui [caricamenti rapidi](mgs-fast-loading.md) ripete tutti i
24 scenari retail dei due CD e aggiunge otto confronti A/B dei tempi del
loader. Passano anche cambio disco, Codec, ripartenza dal salvataggio finale,
entrata e riproduzione completa di un filmato e replay storico. Le misure
e i limiti sono nel [rapporto dedicato](mgs-fast-loading-evidence.json):
non aggiungono una certificazione della campagna o della resa Vulkan.

La precedente batteria del 6 ottobre copre timer, continuità dei savestate, lettura
ISO, allocatore BIOS e controlli strumentati su runtime, analizzatore e
ricompilatore. Prova consolidata:
`data/validation/completion/foundations-20261006/result.json`.
Build finale `5698579a2869fe4f72081e1d6b7d2a3f29c17de110d02dc4221bdc5e00732079`.

- **Timer:** il reset al target generava falsi overflow; gli alias del clock
  0/2 e 1/3 dei timer 0/1 non erano equivalenti; la divisione per otto perdeva
  il resto a ogni chiamata host. Corretti anche arresto del timer 2, lettura
  con azzeramento dei flag MODE, status non scrivibili e IRQ toggle.
  Le matrici coprono tutte le 16 combinazioni IRQ, 48 combinazioni
  timer/sorgente/segnali e suddivisioni del clock da 1 a 17 cicli.
- **Savestate NSV11:** salva senza consumare i flag dei timer e conserva
  interrupt one-shot già scattati, livello toggle e credito frazionario.
  La matrice di continuità verifica 640 configurazioni/fasi; il file viene
  inoltre salvato, riletto e fatto avanzare. Due nuove mutazioni con checksum
  corretto vengono rifiutate senza modificare la partita. I vecchi formati
  restano leggibili, ma non contengono la storia dei timer ora conservata.
- **ISO:** UBSan ha riprodotto letture tramite puntatori non allineati. Il
  parser usa ora letture little-endian per byte, rispetta lunghezza della
  directory, record e nomi, rifiuta extent oltre il CD e cancella la radice
  precedente se la nuova inizializzazione fallisce. I test coprono tutti i
  record corti da 1 a 33 byte e nomi che oltrepassano il proprio record.
  L'audit nativo con ASan/UBSan confronta nomi, LBA e dimensioni con un parser
  Python indipendente: **22 file indicizzati sui due CD**, nessuna differenza.
  Questo verifica directory e ricerca dei percorsi, non tutti i payload.
- **Heap BIOS:** richieste enormi potevano diventare zero durante
  l'allineamento e sovrascrivere i collegamenti dell'allocatore. Vengono
  rifiutate prima di mutare RAM; restano invariati dati già allocati e capacità
  recuperata dopo la liberazione. Il nuovo test falliva prima della correzione.

**1.297 test runtime passati, 31 esclusi**, sia nella build ordinaria sia
sotto ASan/UBSan: sono gli stessi contratti, non 2.594 verifiche indipendenti.
Passano inoltre **129 test del ricompilatore** (uno escluso), **121
dell'analizzatore**, entrambi strumentati, e **138 test Python**. Conservati
anche i test rossi: sei sui timer/savestate, tre ISO, uno heap e il rapporto
UBSan originale. La build strumentata riguarda i test, non tutto l'eseguibile
del gioco ricompilato. LeakSanitizer è disabilitato perché non funziona sotto
ptrace in questa sessione; ASan segnala supporto incompleto per
`makecontext/swapcontext`. Non vengono quindi certificati leak o ogni accesso
alle stack delle coroutine.

Regressioni di gioco riuscite, con identità dei binari conservate:

| Prova | Esito | Build |
| --- | --- | --- |
| Codec al molo | Apertura/chiusura native e donor; replay esatto di 146 frame | `0dc74c17…`, correzione timer |
| Cambio disco | Originale s12c → s13a, un cambio e una validazione, nessun Start; replay esatto di 745 frame | `0dc74c17…` |
| Ripresa NSV11 e prima persona | Nuovo processo, coppia native/donor e replay esatto di 48 frame | `5698579a…`, finale |
| Load retail sul CD2 | s18a/277 con Liquid controllabile; gli endpoint concordano con il donor storico dello stesso contratto | `5698579a…` |
| Replay filmato da NSV10 | 1.460 frame, nessuna desincronizzazione RAM/continuazione | `5698579a…` |

Le prove native riuscite non registrano errori di gioco né interprete. Nessun
hash delle carte personali o del savestate personale è cambiato. Il manifesto
resta a **130 contratti**: questi controlli di regressione non aggiungono
porzioni indipendenti di campagna certificata.

Per la presentazione passano i contratti dei **60 adattatori** e `spirv-val`
sui **30 moduli SPIR-V** compilati. Sono verifiche statiche; l'accesso SDL al
dispositivo video resta indisponibile e non ci sono nuove prove visive Vulkan.
Gli shader non sono stati modificati. La politica resta la resa generale
moderna lato presentazione, con simulazione originale sotto il cofano.

I timer sono confrontati con il donor fissato in
`donor/source.json` (`src/core/timers.cpp`) e con i
[registri documentati da PSX-SPX](https://psx-spx.consoledev.net/ps1/system/timers/).
Non è una riscrittura cycle-accurate: sincronizzazione blanking dei timer 0/1,
generazione dotclock, target zero e dettagli temporali al limite del contatore
restano da verificare; la convenzione precedente di wrap a 16 bit è conservata.
La campagna completa e l'equivalenza audiovisiva rimangono non certificate.

```bash
# Audit read-only riproducibile; usare una destinazione nuova.
python3 scripts/mgs_iso_audit.py data/validation/completion/iso-next
# Dopo la configurazione con PS1RECOMP_ASAN=ON e PS1RECOMP_USE_STUB=ON:
ASAN_OPTIONS=detect_leaks=0 cmake --build build-sanitize --target ps1Runtime_tests
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  build-sanitize/ps1Test/ps1Runtime_tests
```

La precedente batteria del 6 ottobre estende la verifica a filmati CD1, varianti di
overlay, integrità delle memory card e rifiuto dei savestate invalidi. Prova:
`data/validation/completion/systematic-storage-20261006/result.json`.

**Filmati CD1:** nove coppie recomp/SwanStation completano quattro asset
distinti e le callback originali. Il controllo ricava i conteggi dal CD:

| Filmato | Scene verificate | Fotogrammi per esecuzione | Settori XA letti |
| --- | --- | ---: | ---: |
| `38d0` | `s04b`, `s04c`, `s04br` | 782 | 979 |
| `6137` | `s08b`, `s08br` | 388 | 486 |
| `2302` | `s08b`, `s08br` | 625 | 783 |
| `97ed` | `s08b`, `s08br` | 412 | 645–646 |

Ogni prova passa anche macroblocchi completi, audio consumato, assenza di
troncamenti MDEC e credito CD perso. Tutti gli otto asset dei due CD hanno ora
una prova di riproduzione nativa completa sui binari registrati; non sono
ancora verificate tutte le combinazioni di scena, mix, lingua o presentazione.
Le nove coppie non sono nove filmati diversi né nove porzioni indipendenti di
campagna. Le nuove prove usano runtime `46208d1a…` e `1f73d75b…`; non vengono
ridenominate come esecuzioni della build successiva del loader.

`mgs_movie_paths.py` legge i rami originali, conserva condizioni e offset e
genera carte isolate modificando solo scena/storia. Gli `else-if` escludono
esplicitamente le condizioni precedenti. Rami annidati o dipendenti da variabili
non supportate restano inventario, senza una preparazione indovinata.
Il nome della scena può cambiare mentre `abst.c` deve ancora essere confermato
o ricreato: l'input di conferma è quindi armato sulla presenza di quell'actor e
si arresta alla creazione di `movie.c`. La callback `ad2d` riavvia lo script,
che può avanzare immediatamente la storia; l'uscita usa il limite inferiore
originale insieme all'assenza degli actor e ai conteggi completi del video.
Le prove preliminari fallite per questi errori del preparatore restano conservate.

**Memory card:** corretto l'accesso a catene corrotte che potevano coinvolgere
i blocchi di un altro file. Lettura/scrittura/cancellazione verificano checksum
directory, ruoli testa/intermedio/coda e unicità dei collegamenti. I test
coprono tutti i 255 stati invalidi della testa, catene condivise, sette blocchi
frammentati, ogni confine di blocco, pieno e riapertura da disco. La prova
originale di salvataggio finale passa di nuovo; la carta realmente scritta
viene riletta da un nuovo processo nella recomp e nel donor: `s00a/6`, vita
256/256, CD1. Nessuna carta personale viene usata in scrittura.

**Savestate:** un checksum corretto non garantiva una struttura valida. Il
loader poteva modificare CPU/RAM e poi rifiutare un campo finale; accettava
anche alcuni payload di lunghezza errata. Ora prepara i dati separatamente,
valida struttura, dimensione esatta, input e BIOS, quindi seleziona il disco e
applica il risultato. I rifiuti controllati precedono le modifiche alla partita.
I test includono sei mutazioni con checksum ricalcolato e il mantenimento del
disco corrente in entrambe le direzioni. La build finale
`023acb63c60b8d383f20396f1ac9480a0384a3cf28a44570758464ce997df331`
passa il replay NSV10 di 1.460 frame e carica una copia del savestate personale
NSV7 per 240 VBlank, con 119 avanzamenti di `GV_Time` e nessun interprete/errore.

**1.284 test runtime passati su 126 suite, 31 esclusi; 137 test Python passati.**
Il tentativo separato di eseguire la batteria grafica non costituisce una prova
Vulkan riuscita: 22 test si fermano all'inizializzazione SDL (`No available video
device`); anche il tentativo X11 fallisce prima del renderer. Le nove verifiche
non grafiche passate in quel filtro si sovrappongono alla suite ordinaria e non
vengono contate due volte. La fedeltà visiva resta una verifica aperta.

La precedente verifica media del 6 ottobre copre l'intero archivio
`ZMOVIE.STR` dei due CD USA e aggiunge la riproduzione integrale dei crediti.
Gli attesi vengono ricavati dagli indici e dai settori originali, senza usare
i conteggi osservati nel runtime come obiettivi del test.

- **8 filmati, 9.150 fotogrammi codificati**: indice, limiti delle porzioni,
  intestazioni duplicate XA, ordine e completezza dei chunk, dimensioni e hash
  dei payload verificati su entrambi i dischi. I chunk interamente di padding
  sono legali; il numero di chunk non coincide necessariamente con la lunghezza
  compressa. `97ed` usa circa 12 fps, gli altri circa 15: nessuna assunzione
  universale di 15 fps nel catalogo.
- **11.571 settori XA, 27.214.992 frame PCM stereo**: confronto offline esatto
  con decoder e ricampionatore SwanStation per tutti gli otto filmati. A ogni
  confine di settore una seconda istanza ripristina lo snapshot del decoder e
  deve produrre gli stessi campioni; anche la storia successiva resta confrontata
  con il donor. Questo esercita il decoder, non il caricamento del file NSV.
- **Crediti, telefonata finale e rank completati** nella recomp e nel donor.
  Il percorso originale `roll` usa `22b4` e `ee52`: **5.337 fotogrammi** e
  **1.067.400 macroblocchi**, corrispondenti agli attesi. Il controller decodifica
  **6.675 settori XA**, entro l'intervallo 6.670–6.678 derivato dai settori che
  precedono l'ultimo frame utilizzato e dal read-ahead massimo nell'archivio.
  Tutti i **15.699.600 frame PCM** prodotti vengono consumati dallo SPU.
  Nessun comando MDEC troncato, credito CD scartato, errore di gioco o interprete.
- Corretto un difetto generale MDEC: l'esaurimento dei dati non conclude più
  implicitamente un blocco RLE. Rimangono disponibili soltanto i macroblocchi
  completi. Un comando vuoto o parziale non incrementa `mdec.decoded_frame`;
  `mdec.decode_command` e `mdec.truncated_command` rendono distinguibili i casi.
  Due nuovi test fallivano prima della correzione. La matrice copre ogni taglio
  di parola di un flusso con due macroblocchi, RGB15/24, trasferimenti DMA
  suddivisi e snapshot durante l'ingresso dei dati.

La build verificata è `46208d1a9057fe758030346eddad3da6c2fd748d1153ff654508161bcdef82b3`:
**1.280 test runtime passati**, 31 opt-in esclusi, **131 test Python passati**.
Il replay di 1.460 VBlank del filmato ripreso da NSV10 verifica i testimoni RAM
e di continuazione registrati sulla build precedente. Prova consolidata:
`data/validation/completion/media-20261006/result.json`.

La durata totale della nuova prova crediti è 28.609 VBlank contro 29.635 donor,
comprendendo avvio e Load. Allineando l'inizio effettivo dei crediti, la fase
fino al flag della telefonata dura **21.605 contro 21.528 VBlank**; il tratto
successivo fino a rank **3.613 contro 3.596**. Non sono durate identiche e non
certificano sincronismo labiale o ascolto comparato. Il controllo dei conteggi
è aggregato: non identifica ogni immagine effettivamente presentata.

Comandi riproducibili, usando nuove destinazioni per conservare le prove:

```bash
python3 scripts/mgs_movie_catalog.py DISC1.bin DISC2.bin --output NEW/catalog.json
python3 scripts/mgs_xa_audit.py NEW/catalog.json NEW/xa --catalog
python3 scripts/mgs_movie_check.py NATIVE/result.json DONOR/result.json \
  --catalog NEW/catalog.json --movies 22b4 ee52 --output NEW/credits.json
```

Il controllo playback rifiuta un CD diverso dal catalogo, contatori mancanti,
fotogrammi o macroblocchi mancanti/eccedenti, troncamenti, perdita di clock e
PCM prodotto ma non consumato. Per usarlo la prova deve iniziare prima dei
filmati dichiarati e terminare dopo lo svuotamento della coda audio. Il cutoff
di due frame viene da `movie.c` e `ending2.c`; non è applicabile arbitrariamente
ad altri lettori. La dichiarazione dei due filmati dei crediti è riscontrata
negli script retail `roll/ea54` e `roll/a242`, procedura `main`.

L'inventario e l'oracolo audio coprono tutti gli asset. Questa precedente prova
certificava la riproduzione dei quattro filmati CD2; la batteria successiva
descritta sopra aggiunge i quattro CD1. Restano aperti altri actor/overlay, mix
audiovisivo e revisione Vulkan. La campagna completa resta non certificata.

La precedente verifica del 6 ottobre completa il percorso **audio XA dei filmati**
e corregge il rallentamento della lettura CD. Il controller consegnava i settori
XA alla CPU come dati; il collegamento audio non veniva invocato. Inoltre, il
cambio task interrompeva la consegna dei settori già maturati al driver originale,
limitandola spesso a uno per VBlank. La correzione completa le transazioni
CD/DMA prima del cambio task, senza aggiungere tempo artificiale al drive.

- Primo filmato `e80c`: **480 fotogrammi**, 601 settori XA e 1.413.552 frame PCM
  stereo; **1.971 VBlank nativi contro 1.973 donor**, rispetto ai precedenti 2.993.
  Nessuna perdita di credito del clock CD nella prova corrente.
- Introduzione completa di Liquid: **1.590 fotogrammi**, 1.990 settori XA,
  4.680.480 frame PCM; **19.887 VBlank nativi contro 19.916 donor**, fino al
  controllo nello scontro. Nessun credito del clock CD perso.
- Savestate NSV10 a metà filmato, ricaricato in un nuovo processo: **120 + 360
  fotogrammi**, **150 + 451 settori XA**; stessi totali audio/video della prova
  continua. Anche SwanStation completa i due segmenti.
- Audit indipendente dell'intera porzione su CD: **604 settori XA**, 1.420.608
  frame PCM, identici campione per campione al decoder e ricampionatore del
  donor. Il gioco chiude il filmato prima degli ultimi settori della porzione:
  il conteggio offline non è il conteggio atteso della riproduzione in gioco.

Sono implementati XA 4/8 bit, mono/stereo, 18.900/37.800 Hz con uscita stereo
44.100 Hz, filtro file/canale, selezione automatica ed EOF, mute, matrice volume
CD e guadagni CD dello SPU. I registri corretti distinguono interrupt-enable
(indice 1/porta 2), ACK (1/3), matrice CD (2/2, 2/3, 3/1, 3/2) e applicazione/mute
(3/3). `CdMix` HLE usa la sequenza retail USA `CD_mix` a `0x80090824`.
L'input CD continua a essere consumato quando il gioco disabilita il mix, per
non riprodurre audio vecchio alla riattivazione. NSV10 conserva predittori,
ricampionamento, filtri, mute, mixer e PCM pendente; vedi [savestate](mgs-savestates.md).

L'oracolo usa `common/cd_xa.cpp` e `CDROM::ResampleXAADPCM` di SwanStation
`7f69c199ed88d5723f71dd3a6e9c1b7a45b535a6`, rimuovendo solo il controllo della
capienza della coda host per la decodifica offline. Il confronto reale riguarda
XA stereo 4 bit; gli altri modi sono verificati con campioni sintetici. Per gli
8 bit viene usato lo shift di 8 previsto da [PSX-SPX](https://psx-spx.consoledev.net/ps1/cdr/cdromformat/),
correggendo lo shift di 12 riutilizzato dal donor. La mappa dei registri è
confrontata anche con [CDROM drive](https://psx-spx.consoledev.net/ps1/cdr/cdromdrive/).

```bash
python3 scripts/mgs_xa_audit.py \
  'data/discs/usa/disc2/Metal Gear Solid (USA) (Disc 2) (v1.0).bin' \
  data/validation/completion/xa-oracle-new --start-lba 228157 --sectors 4833
```

Prova consolidata: `data/validation/completion/xa-fix-20261006.json`, con hash
runtime `07a37b68949035b7ffcc379970c9193e254c8f072de8d0c1b3d19e35015279a9`.
**1.277 test runtime passati**, 31 opt-in esclusi, **121 test Python passati**.
Il replay TAS del segmento ripreso dal nuovo savestate completa tutti i 1.460
frame con i testimoni RAM e di continuazione originali, senza desincronizzazione.
Savestate e memory card dell'utente conservano i loro hash.

La regressione della tortura completa lo stesso percorso e raggiunge s03c/158,
vita 576, senza resa: **27.281 VBlank nativi complessivi contro 27.068 donor**.
La nuova esecuzione nativa è divisa in due processi: l'harness ha interrotto il
primo al limite di 240 secondi, dopo 24.295 VBlank; il secondo riprende esattamente
checkpoint NSV10 e carte per altri 2.986 VBlank. Il rapporto interrotto resta
`needs-review`; il confronto conclusivo è separato in
`data/scenarios/xa-20261006/torture-segmented.json`. Non viene conteggiato come
nuovo contratto continuo né sostituisce la precedente prova integrale.

La precedente correzione FMV/MDEC resta verificata: DICR mascherato e scritture
parziali, DMA d'uscita in attesa dei pixel, catena dei completamenti MDEC, formato
15/24 bit e matrice IDCT caricata dal gioco. Le istruzioni retail restano
eseguite e non vengono forzati flag di storia nel runtime. Le ricette ora
richiedono sia decodifiche video sia audio effettivamente miscelato dallo SPU:
il solo raggiungimento della fine scena non basta.

La precedente verifica XA riguarda i due filmati di Liquid e i percorsi esplicitamente
elencati nella prova. Non certifica tutti i filmati del gioco, il mix audio
completo, un ascolto comparato o la presentazione Vulkan. I test sono headless;
il criterio grafico resta la resa generale con metodi moderni, definito nel
[contratto Vulkan](mgs-vulkan-presentation.md#criterio-di-fedeltà-e-regressioni).
Le prove precedenti e i fallimenti diagnostici restano conservati con le loro
identità; la campagna completa resta non certificata.

Il 5 ottobre 2026 sono passati **30 nuovi contratti di verifica**, ciascuno
eseguito nella recomp nativa e in SwanStation: 19 morti/Continue e 11 passaggi
di avanzamento, finali e salvataggio. Gli stati finali concordano sui campi
previsti da ogni contratto. Le esecuzioni native riuscite non riportano errori
di gioco o ricorso all'interprete.

Il 6 ottobre si aggiungono **cinque contratti riusciti**: morte/Continue di
Snake18 nello scontro con Liquid, resistenza alla prima sessione di tortura,
resa e morte sotto tortura con uscita al titolo, e ramo introduttivo seguito
dalla prima sessione senza resa. Il blocco dello stream di quest'ultimo percorso
è corretto nella temporizzazione audio nativa. Ogni prova confronta recomp
e SwanStation e dichiara le condizioni iniziali sintetiche. È stato inoltre
ripetuto il caricamento del salvataggio finale dopo la correzione delle
callback, confrontandolo con la precedente esecuzione donor dello stesso
contratto; non viene conteggiato come copertura aggiuntiva. Dopo la correzione
audio sono passati anche l'introduzione di Liquid (nuova coppia) e i crediti
fino a rank; la successiva verifica media descritta sopra sostituisce la prova
dei crediti con una nuova coppia e controlli video/audio. Il manifesto mantiene
i riferimenti precedenti.

**La campagna completa resta non certificata.** Preparare la fase di morte di
un boss prova la sua uscita, ma non il combattimento che la precede. Preparare
un finale prova il suo svolgimento, ma non la tortura o la fuga che lo rendono
raggiungibile. Caricare un salvataggio e muoversi prova ancora meno.

Il [manifesto](mgs-completion-manifest.json) separa queste categorie e fissa gli
hash dei risultati. Il [rapporto riproducibile](mgs-completion-evidence.json)
elenca 29 aree di verifica, 25 parzialmente esercitate e nessuna certificata
integralmente. Contiene 130 contratti riusciti: nove nuovi percorsi FMV CD1, tre verifiche FMV/savestate, i 35 nuovi, i 24 caricamenti di
scena già verificati, 50 esecuzioni precedenti di 48 situazioni distinte,
8 segmenti TAS storici e un replay del cambio disco. Non sono 130 porzioni
indipendenti della campagna e il conteggio non rappresenta una percentuale di
completabilità. I risultati precedenti conservano l'identità del loro runtime;
non vengono presentati come rieseguiti con l'ultima build.

## Nuove prove misurate

| Contratto | Preparazione | Risultato concordante | Limite |
| --- | --- | --- | --- |
| Morte e Continue, 19 scene | Evento letale sintetico nel TARGET di Snake | Morte originale, selezione Continue, controllo stabile, avanzamento e inventario ripristinati | Non certifica ogni causa di morte o le uscite dei capitoli |
| PAL fredda | Salvataggio retail non modificato, chiave già fredda | Movimento, equipaggiamento, terminale: storia 240 → 242 | Acquisizione e raffreddamento non esercitati |
| PAL calda | Salvataggio retail non modificato, chiave già calda | Terminale, filmati e Codec: storia 246 → 250 | Riscaldamento, sopravvivenza al gas e uscita non certificati |
| REX, radome | Fase di morte impostata in copia del checkpoint | Animazione, Ninja e seconda fase: storia 257 | Nessuna prova completa dei danni e dello scontro |
| REX, cockpit | Continuazione della coppia precedente, fase di morte sintetica | Callback originale di vittoria e ingresso in s18a, storia 260 | Non equivale a vincere il combattimento |
| Introduzione di Liquid | Continuazione della precedente uscita di REX | Due FMV osservati fino agli ultimi frame, 1.590 decodifiche, Codec e controllo nello scontro, storia 277 | Ascendenza sintetica; XA e durata verificati sui due video, mix completo e ascolto comparato non certificati |
| Sconfitta di Liquid | Salute e segnale di colpo preparati in copia | Vittoria originale, filmati e controllo in s19a, storia 283 | Non esercita tutto il combattimento o il timer |
| Finale Meryl | Carta isolata con ingresso s20a/286 e flag di ramo | Filmati, Codec, ending e roll/294 | Non prova resistenza alla tortura o fuga in jeep |
| Finale Otacon | Stesso ingresso, flag dell'altro ramo | Filmati, Codec, ending e roll/294 | Non prova resa alla tortura o fuga in jeep |
| Crediti e risultato | Ingresso sintetico roll/294 attraverso Load originale | Telefonata conclusiva e rank/294; 5.337 frame, 1.067.400 macroblocchi, 6.675 settori XA | Conteggi completi; sincronismo audiovisivo e Vulkan non certificati |
| Salvataggio finale | Continuazione dei crediti verificati | Dialogo e sovrascrittura originali, nuovo record rank/0/CD1 con CRC valido, ritorno al titolo | Carte di lavoro isolate |
| Rilettura finale | Nuovo avvio sul CD1, carta realmente scritta dalla recomp | Load originale e molo controllabile: s00a/6, Normal, vita 256/256 | Non certifica tutti gli sbloccabili o le combinazioni di finale |
| Liquid: morte/Continue | Evento letale sintetico nel TARGET puntato da Snake18, work+7E8 USA | Game Over, Continue e controllo stabile in s18a/277 | Non prova vittoria o scadenza del timer |
| Tortura: resistenza | Carta isolata s03b/157, intro già vista; successivamente solo input | Prima sessione completata, conteggio0065=1, resa005c bit8=0, ritorno s03c/158 | Non prova cattura, intro o sessioni successive |
| Tortura: resa | Ingresso sintetico s03b/157; input originali anche di salto intro | Flag005c bit8=1, conteggio0065 incrementato, cella s03c/158 | Non prova continuità fino al finale Otacon |
| Tortura: morte/Exit | Vita sintetica 1 nell'actor originale, poi shock e menu originali | Game Over senza Continue; titolo corrente e open.c attivo, caricamento concluso | La scena salvata resta s03b, non identifica il menu corrente |
| Tortura: ramo introduttivo e resistenza | Stesso seme e percorso di input del blocco riprodotto; primo actor saltato con Cross, poi solo Circle durante il filmato e gli shock | Filmato supera il precedente stallo, prima sessione conclusa senza resa, cella s03c/158 | Non prova cattura continua, primo actor senza salto o sessioni successive |

Le 19 scene della morte/Continue sono `s01a`, `s03a`, `s04b`, `s06a`, `s07c`,
`s08c`, `s09a`, `s10a`, `s11b`, `s11c`, `s12b`, `s12c`, `s13a`, `s14e`,
`s15a`, `s16a`, `s16d`, `s17a`, `s19a`. La prova aggiuntiva in `s18a` usa
una ricetta specifica per Snake18: il TARGET è indiretto e non coincide con
quello incorporato usato da Snake ordinario.

I salvataggi e checkpoint dell'utente sono rimasti immutati:
`sha256sum -c .local/tas-protected.sha256` passa; ogni coppia di situazioni
controlla inoltre gli hash dei propri semi originali e delle carte.

## Problemi corretti

Il percorso dei crediti chiamava `DG_GetDisplayEnv` all'indirizzo USA
`0x80018008`, interno a un intervallo che l'analizzatore attribuiva alla
funzione precedente. L'assenza di un ingresso nativo provocava chiamate
all'interprete privato. `prepare_native.py` ora verifica le quattro istruzioni
retail del leaf e registra l'alias nativo. La prova completa dei crediti passa
con zero chiamate all'interprete.

Il controllo dei dispatch costanti di tutti gli overlay compilati ha trovato
altri dieci ingressi mancanti, ora registrati:
`80027A44`, `80027F58`, `80032498`, `800627B0`, `80081BE0`, `80081C20`,
`80081CF8`, `8008F8F4`, `80095360`, `80095F40`.
Il codice eseguito rimane quello MIPS originale ricompilato; non vengono
forzate vittorie o modificati gli script di avanzamento nel runtime.
L'audit finale copre **806 siti di dispatch costante, 77 destinazioni distinte,
160 unità generate, zero ingressi main-text irrisolti**. Non copre le chiamate
indirette né dimostra che tutti i percorsi possibili siano stati eseguiti.
Tre valori distinti fuori dal testo eseguibile restano separati come non
certificati, senza reinterpretare arbitrariamente tabelle incorporate come codice.

L'audit aggiuntivo degli overlay verifica **73 overlay configurati, 1.514
riferimenti di costruttore e 6.620 candidati callback**. Esclude gli indirizzi
validi soltanto come continuazioni dei savestate e simula la prima identità
che il dispatcher selezionerebbe, senza unire impropriamente tabelle diverse.
Dopo la correzione risultano zero ingressi mancanti e zero collisioni delle
identità; i binari configurati coincidono con quelli dei due dischi retail.
I 21 overlay non compilati restano elencati separatamente nell'artefatto.
L'audit è conservativo e non certifica ogni chiamata indiretta possibile.

Sono stati aggiunti due ingressi alla generazione riproducibile:

- `title:800D5F30`, distruttore di `met_logo.c`: esegue lo store originale di
  `DG_FrameRate=2` dopo la presentazione del logo, che imposta il valore 3.
- `opening:800C7F6C`, distruttore vuoto di `blurpure.c`: registrato
  esplicitamente, senza attribuirgli un crash non osservato.

Si tratta dei leaf MIPS retail ricompilati; la logica originale non viene
sostituita. La prova di rilettura del salvataggio finale passa dopo la modifica
con zero ricorso all'interprete. Questo non dimostra che entrambi i leaf siano
stati attraversati in quella specifica esecuzione.

Negli strumenti di prova:

- `current_stage` osserva `GM_CurrentStageName` a `800ADB34`; il vecchio
  `stage` conserva il significato storico di scena salvata a `800B7500`.
  Confondere i due causava un falso fallimento dopo la morte sotto tortura:
  il titolo era già caricato ma la scena salvata rimaneva `s03b`. Entrambi i
  motori passano ora il contratto corretto; il tratto dall'animazione del Game
  Over al menu richiede 440 VBlank su entrambi. L'osservazione dell'actor del
  titolo usa anch'essa la scena corrente.
- I campi GCL richiesti soltanto per confronto o conservazione vengono
  acquisiti prima di validarli. Prima il verificatore li respingeva come
  sconosciuti se non comparivano anche nei predicati del percorso.
- Selezionare il CD2 ora mantiene il CD1 nell'insieme dei dischi; prima poteva
  duplicare il CD2 nella configurazione temporanea.
- Una matrice interrotta o priva di `status` ora fallisce la verifica. Il vecchio
  aggregato `death-disc1-02` è stato rettificato con nota della correzione: due
  casi incompleti erano esclusi dal conteggio dei problemi. Entrambi sono poi
  passati in `death-disc1-legacy`; il rapporto cita i risultati individuali.
- Il donor usa il TasRoute condiviso, compilato in C++20, con predicati degli
  actor e budget derivato dalla ricetta. I vecchi semi senza identità completa
  richiedono un harness esplicito; la lacuna storica resta annotata.
- I risultati fissano checkpoint, RAM, carte, configurazione e identità di
  esecuzione. Una continuazione verifica checksum, testimone RAM e provenienza
  della coppia. La presenza di un salvataggio già esistente non basta a far
  passare la prova di scrittura: occorre un nuovo record valido.

## Struttura originale e priorità residue

Il catalogo GCL dei due CD decodifica **274 script, 13.142 procedure e 192
record di scenario**, corrispondenti a 96 nomi distinti. Registra 1.718
destinazioni letterali di caricamento e 1.540 assegnazioni della storia,
con zero errori di parsing. Include condizioni lessicali, risorsa, offset e
hash; usa i comandi e il layout documentati dalla decompilazione
[FoxdieTeam](https://github.com/FoxdieTeam/mgs_reversing), commit
`7b66372bcf156249560c90918d9472a47aabf3c5`, verificando i binding sulla USA.
Il layout USA ha 95 variabili linked, contro le 96 di Integral: le due
strutture di salvataggio non sono intercambiabili.

È un inventario sintattico, non un grafo di raggiungibilità dimostrato. I
duplicati tra dischi e le varianti inutilizzate non diventano scenari distinti
da certificare. I riferimenti a stringa vuota, `d04b`, `dummy`, `s11f`,
`s14a`–`s14d`, `titlea`, `titlep` richiedono ancora classificazione rispetto ai
rami retail/debug: la sola assenza dal catalogo non dimostra un bug.

Il manifesto elenca i controlli residui per capitolo. Priorità:

1. Cattura continua, primo actor della tortura senza salto, sessioni successive,
   fuga dalla cella e recupero dell'equipaggiamento.
2. Jeep: garage, barriere, inseguimento e vittoria con entrambi i compagni.
3. Mantis: controller 2 e alternativa delle statue; Nikita e quadro elettrico.
4. Combattimenti completi e danni: carroarmato, Ninja, Wolf, Hind, Raven, REX e
   Liquid. La vittoria storica su Ocelot parte da uno scontro già in corso.
5. PAL: recupero, neutralizzazione, cambi di temperatura, gas e uscita.
6. Collegamenti e rischi ambientali non coperti dai semplici caricamenti,
   salvataggi in condizioni di errore e sbloccabili di entrambi i finali.

Le etichette retail `system -s` hanno corretto cinque attribuzioni nel
manifesto: `s06a` è Nuke Bldg 1, `s09a` Cave, `s10a` Underground passage e
`s11c` Comm Tower B. Questi caricamenti non provano rispettivamente fuga dalla
cella, Mantis, inseguimento sulla torre o Hind. I risultati originali e i
loro hash non sono stati cambiati.

## Blocco dello streaming corretto

La prova storica `torture-resist-03` resta conservata come fallimento: frame
`demothrd.c` fermo a 3.184, clock dello stream a 10.445, `str_status=5`.
Il buffer circolare FS aveva un sottotitolo alla testa, in attesa che il clock
audio raggiungesse il tempo di rilascio. Esauriti i pacchetti audio, il clock
non avanzava più; quel sottotitolo impediva al lettore CD di riempire il buffer.

La traccia ha identificato aggiornamenti persi nel driver audio: in 300 VBlank,
492 interrupt SPU ma solo 446 chiamate a `StrSpuTransWithNoLoop`. I punti di
cooperazione nativi potevano consumare un secondo interrupt prima che `SdInt`
finisse il precedente. `mts_isend` scarta la notifica se il ricevitore è già
READY; `MTS_TASK_INTR` non la accoda. Il cursore di riproduzione procedeva così
più rapidamente del contatore usato da filmato e sottotitoli.

`Bios::drainPendingCallbacks` ora conserva il credito SPU finché il task audio
originale è READY/running. Il vincolo è limitato al profilo USA e alla sua
callback `80085150`; il binding MTS (`8008C590`, task `800C33B0`, stato 3)
è verificato sul codice retail ricompilato. Non scrive contatori nella RAM del
gioco e non modifica script, sottotitoli o esiti della tortura.

Il nuovo percorso usa **lo stesso programma di input compilato** della prova
fallita. La recomp completa filmato, prima sessione e ritorno alla cella in
27.297 VBlank, contro 27.068 del donor. Concordano scena/storia `s03c/158`,
vita 576, conteggio delle sessioni 1 e flag di resa assente. Allo stesso
checkpoint di 7.200 frame registrati, il filmato
è a 3.506 e il clock audio a 11.505, uguale al numero degli interrupt: il
precedente stallo è superato. Una ripresa separata a velocità normale per
600 VBlank registra 984 interrupt e 984 chiamate al lavoro del driver.
Il dump RAM di questa sonda avviene durante la chiusura e non è trattato come
un endpoint TAS sincronizzato.

Le regressioni includono introduzione di Liquid fino al controllo nello
scontro e avanzamento dei crediti senza input di salto, telefonata finale e rank. La successiva indagine FMV limita questa prova storica alla progressione semantica. Tutte le esecuzioni
native riuscite riportano zero errori di gioco e zero ricorso all'interprete.
Questa era la differenza di durata della build precedente. Le prove XA in
apertura correggono lettura e durata dei due FMV; il vecchio contratto semantico
con salti resta da interpretare con i limiti della sua build e della sua ricetta.
I savestate già bloccati conservano gli aggiornamenti storicamente persi:
per verificare la correzione si riparte da un checkpoint precedente allo stallo.

Risultati, identità, tracce e confronto dei checkpoint sono fissati in
`data/validation/completion/stream-clock-fix-20261006.json`; il manifesto
conserva il problema fra i `resolved_findings` e mantiene le prove fallite.
I test runtime passano **1.255 casi**, con 31 prove opt-in escluse; passano
anche i 116 test Python. Due nuovi test coprono il vincolo sul ricevitore e
la conservazione di credito/interrupt attraverso il servizio BIOS.

## Riproduzione

Le ricette riuscite sono in `scripts/fixtures/completion/`. Usare destinazioni
nuove: gli strumenti rifiutano di sovrascrivere le prove. I dati retail, le
carte e i checkpoint rimangono in `data/`, esclusi da Git.

```bash
# Catalogo sintattico e controllo degli ingressi del codice già generato.
python3 scripts/mgs_completion.py catalog data/validation/completion/retail-catalog.json
python3 scripts/mgs_completion.py static-entries data/validation/completion/static-main-entries.json
python3 scripts/mgs_overlay_audit.py data/validation/completion/overlay-audit-next.json

# Ricostruisce il rapporto e controlla gli hash fissati nel manifesto.
python3 scripts/mgs_completion.py report docs/mgs-completion-manifest.json docs/mgs-completion-evidence.json

# Una coppia REX già riuscita può essere continuata senza ripetere l'inizio.
python3 scripts/mgs_situations.py continue \
  data/scenarios/completion/rex-radome-02/result.json \
  data/scenarios/completion/rex-cockpit-next \
  --recipe scripts/fixtures/completion/rex-cockpit-exit.json \
  --donor-runner .local/libretro_completion --period-us 1000 --timeout 180

# Crea solo una carta isolata di fase; l'esecuzione passa poi dal menu retail.
python3 scripts/mgs_completion.py prepare-torture \
  data/scenarios/usa-normal/cases/d1-10-s10a-151/cards/card1.mcd 10 \
  data/scenarios/completion/torture-next-case
# Aggiungere --include-intro per il ramo introduttivo ora verificato.

# Ripete esattamente il percorso che prima si bloccava nello stream.
python3 scripts/mgs_situations.py \
  data/scenarios/systematic-20261006/torture-entry/native-01/result.json \
  data/scenarios/systematic-20261006/torture-entry/donor-01/result.json \
  data/scenarios/completion/torture-intro-next \
  --recipe scripts/fixtures/completion/torture-intro-resist.json \
  --donor-runner .local/libretro_systematic --period-us 1000 --timeout 360

python3 scripts/mgs_completion.py prepare-phase \
  data/scenarios/usa-normal/cases/d2-10-s19a-283/cards/card1.mcd 10 credits \
  data/scenarios/completion/credits-next-case
python3 scripts/mgs_scenarios.py run data/scenarios/completion/credits-next-case \
  data/scenarios/completion/credits-next/native --engine native --period-us 1000 --timeout 300
python3 scripts/mgs_scenarios.py run data/scenarios/completion/credits-next-case \
  data/scenarios/completion/credits-next/donor --engine donor \
  --donor-runner .local/libretro_completion --timeout 300
python3 scripts/mgs_scenarios.py compare \
  data/scenarios/completion/credits-next/native/result.json \
  data/scenarios/completion/credits-next/donor/result.json

# Rilegge la carta scritta da una coppia riuscita di salvataggio finale.
python3 scripts/mgs_completion.py prepare-clear-reload \
  data/scenarios/completion/clear-save-02/result.json \
  data/scenarios/completion/clear-reload-next-case

# Matrice morte/Continue, opzionalmente limitata a scene specifiche.
python3 scripts/mgs_completion.py death-matrix \
  data/scenarios/completion/death-next --stages s13a s15a
```

L'harness aggiornato `.local/libretro_completion` è compilabile senza
ricostruire il core SwanStation:

```bash
c++ -std=c++20 -O2 -Wall -Wextra \
  -I.local/swanstation/dep/libretro-common/include \
  -DPS1_DONOR_TAS_ROUTE -Iengine/ps1Runtime/include \
  engine/tools/donor_trace/libretro_headless.cpp \
  engine/ps1Runtime/src/input/tas_route.cpp engine/ps1Runtime/src/metrics.cpp \
  -pthread -ldl -o .local/libretro_completion
```

Validazione del codice al 6 ottobre: **1.253 test runtime passati**, 31 opt-in esclusi;
**129 test ricompilatore passati**, uno escluso; **116 test Python passati**.
I test Python includono rigetto di prove mancanti/manomesse, scritture di
fixture limitate e motivate, checksum dei checkpoint e delle carte, audit
statico vuoto o con ingressi mancanti, conservazione della configurazione CD1/CD2.
Le nuove prove di completabilità sono headless; le verifiche Vulkan del cambio
disco e delle scene restano nei rispettivi rapporti.

La prova storica aveva una discrepanza di temporizzazione nell'introduzione di Liquid: il primo
segmento `liquid-speech-and-movies` impiega 10.430 VBlank nativi contro 16.039
nel donor, con 17 contro 105 cicli di input armati. I successivi segmenti Codec
e ingresso nello scontro differiscono di soli 8 e 3 frame. Lo stato finale
concordante prova l'avanzamento; non certifica la temporizzazione audiovisiva.
L'indagine FMV descritta all'inizio ha identificato timeout e mancata decodifica;
le durate storiche qui riportate non descrivono la build corretta.

La precedente build, usata per introduzione di Liquid, scrittura/rilettura finale,
ha SHA256 `4de4f037c8634a9481dfa699434d15cf8d4908e6623525910ef144fcbd3c9c3e`.
Ogni risultato registra autonomamente il binario effettivamente utilizzato.
