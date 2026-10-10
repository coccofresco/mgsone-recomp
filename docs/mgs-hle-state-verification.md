# Continuità delle risorse BIOS nei savestate

La verifica del 6 ottobre 2026 individua una discontinuità comune a F9 e ai
checkpoint TAS: RAM e dispositivi tornavano al momento salvato, ma file aperti,
cursori, enumerazione delle directory e allocatori BIOS restavano nel processo
abbandonato. In un nuovo processo queste risorse non esistevano. Anche il
generatore casuale BIOS conservava un seme globale non incluso nel file.

## Correzione

Il formato NSV12 conserva i metadati degli allocatori utente e kernel, i 16
descrittori BIOS, i dati CD già letti, i cursori dei file e delle directory e
il seme casuale. I descrittori delle memory card conservano slot e blocco;
al ripristino vengono collegati al controller del processo corrente. Il seme
casuale appartiene ora all'istanza BIOS.

Non vengono riaperte immagini CD o scritte carte per ricostruire i descrittori.
La lista dei blocchi heap resta quella della RAM ripristinata: richiamare
InitHeap l'avrebbe distrutta. Le verifiche di struttura e dei collegamenti
necessari precedono la sostituzione della RAM e la selezione del disco.
I limiti e l'ordine dei campi sono documentati nel
[formato dei savestate](mgs-savestates.md).

Le modifiche sono comuni al BIOS, senza selezione di scena, boss o frame.
I vecchi formati restano leggibili. I campi mai registrati vengono inizializzati
a risorse vuote e seme 1; non è possibile ricostruire la loro storia precedente.
Il prefisso RAM a byte 476 e l'identità del disco negli ultimi 64 byte restano
stabili; anche gli strumenti TAS e di preparazione delle fixture leggono NSV12.

## Matrici di regressione

- Stesso BIOS e BIOS ricreato: prosecuzione delle due allocazioni, contenuto
  delle allocazioni precedenti e sequenza di 32 numeri casuali.
- Lettura concorrente di un file CD e di un file su ciascuna carta, con
  scansione della directory interrotta e ripresa dal risultato successivo.
- Scrittura su controller sostituito: entrambe le carte a offset 0, 127, 128,
  8191, 8192, 16383 e fine file 16384; la carta del controller precedente
  deve restare intatta.
- Tabella piena di 13 file disponibili, cursori distinti e riuso del solo
  descrittore chiuso; sei risorse card invalide devono lasciare i file intatti.
- Quattordici mutazioni del payload con checksum ricalcolato: heap e arena
  impossibili, slot riservati, tipo sconosciuto, lunghezze eccessive o senza
  dati, cursori inconsistenti, directory fuori limite e coda alterata.
  La RAM, i registri e i pulsanti attivi non devono essere sostituiti.
- Controller mancante: il ripristino viene rifiutato prima della RAM; collegato
  il controller, lo stesso file deve tornare leggibile.
- Migrazione NSV6/8/10/11, rimozione delle risorse dell'esecuzione abbandonata
  e conservazione dei dati non RAM durante la preparazione di fixture NSV12.

Le prime due prove di continuità e quella RNG sono state eseguite prima dei
rispettivi fix e fallivano. I JSON e i log originali restano nell'archivio.
I casi che passavano già sono protezioni di regressione, non ulteriori bug.

## Esito della build finale

Build `d5c5168bcda861d5c6c413c419760968fd9d981fb00377dea954084116a327b7`:
**1.325 test runtime passati**, 31 esclusi, sia ordinari sia con ASan/UBSan;
**142 test Python passati**. Sono nove contratti runtime aggiuntivi. La raccolta
iniziale dei test sanitizzati richiedeva di disabilitare LeakSanitizer per il
limite ptrace dell'ambiente; raccolta e suite finali passano con questa opzione.

| Regressione | Risultato |
| --- | --- |
| Codec da checkpoint precedente | Native e SwanStation concordi; replay esatto di 146 frame |
| Prima persona da nuovo NSV12, processo separato | Native e SwanStation concordi; replay esatto di 48 frame |
| Cambio disco originale | Un cambio e una validazione, s12c → s13a; replay esatto di 504 frame |
| Filmato da checkpoint storico NSV10 | 1.460 frame senza divergenza RAM/continuazione |
| Caricamento retail CD2 s18a/277 | Controllo e accovacciamento/rialzata; loader init/title/abst/s18a in 4/4/4/5 VBlank |

Le otto esecuzioni native includono i replay. Tutte terminano senza errori di
gioco e senza ricorso all'interprete. Gli ingressi Codec/prima persona ereditano
la precondizione sintetica della verifica di ricaricamento del salvataggio finale;
non dimostrano il percorso di campagna che conduce a quel salvataggio.
Le carte e il savestate personali mantengono i loro hash.

## Riproduzione e limiti

```bash
cmake --build build --target ps1Runtime ps1Runtime_tests --parallel 4
build/ps1Test/ps1Runtime_tests --gtest_filter='BiosContinuationTest.*:FileIOTest.*:SaveStateTest.*'
python3 -m unittest discover -s scripts/tests
```

Il [rapporto della build verificata](mgs-hle-state-evidence.json) raccoglie hash,
test completi, prove contro SwanStation, replay e controllo delle carte personali.
Gli artefatti sono in `data/validation/completion/hle-state-20261006/`.
Le prove definitive usano il prefisso `final-`; quelle precedenti appartengono
all'iterazione prima dell'inclusione del generatore casuale e non certificano
il formato NSV12 finale.

Le verifiche di gioco sono headless: non certificano la resa Vulkan, la latenza
del monitor o l'intera campagna. I sanitizzatori coprono i test con gioco stub;
LeakSanitizer è disabilitato e ASan ha supporto incompleto per makecontext e
swapcontext. I vecchi checkpoint non possono recuperare metadati non salvati.
La correzione non completa il protocollo seriale delle memory card o i tempi SIO.
