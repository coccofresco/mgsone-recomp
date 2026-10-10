# Verifica delle transazioni dei dispositivi e degli ingressi

Le verifiche del 6 ottobre 2026 estendono i test dal dispositivo isolato al
percorso `Memory` usato dalle istruzioni ricompilate. Gli scenari di gioco non
attivano necessariamente ogni ampiezza di accesso: una periferica può superare
il proprio test e restare irraggiungibile da `LH`, `LW` o `SB`.

## Difetti riprodotti e correzioni

- I timer a 16 bit erano raggiungibili con halfword, ma `LW` restituiva zero e
  `SW` non li modificava. Ora i tre contatori, MODE e target ricevono anche
  accessi word. Le letture byte/halfword selezionano la porzione richiesta
  dopo una sola lettura del registro: MODE consuma i flag una sola volta,
  anche quando viene letta la metà alta.
- Una scrittura halfword ai registri DMA veniva spezzata in due byte. Il
  primo poteva eseguire un comando intermedio prima di ricevere il secondo.
  Il test arma MDEC-out senza dati, rende disponibile un macroblocco e
  annulla il trasferimento: prima della correzione venivano consumati i
  pixel e sovrascritta la RAM di destinazione. Ora l'annullamento conserva
  RAM, FIFO e contatore dei trasferimenti. Le letture halfword sono anch'esse
  una singola transazione.
- Gli accessi word al CD raggiungevano solo la prima porta. Ora le quattro
  porte vengono visitate in ordine, con un'operazione per byte. Sono
  controllati consumo della risposta e del settore, selezione dell'indice,
  caricamento/applicazione della matrice audio e abilitazione degli interrupt.
- JOY_DATA non riceveva accessi byte o halfword dal bus; JOY_MODE non
  riceveva accessi word. Ora le ampiezze convergono sui medesimi registri;
  sono controllate anche le letture per byte di MODE/CTRL/BAUD.
  Le prove inviano pacchetti digitali e analogici completi su entrambe le
  porte e sui tre alias degli indirizzi.
- `PadGetState` e `PadInfoMode` osservavano il tipo fisico immediato anche
  durante il campionamento TAS, mentre i buffer diretti conservavano quello
  del fotogramma. Ora il tipo diventa visibile al confine VBlank, come i
  pulsanti e gli assi. `PadRead` non restituisce più pulsanti premuti da una
  porta dichiarata scollegata.

Le modifiche sono comuni ai dispositivi, senza condizioni sul livello, sul
boss o sul fotogramma. Il formato dei savestate rimane invariato.

## Esito sulla build finale

Build `9420f2b967bcd110b272c7372f090baef7f20e1189e5b579faf5ca7b82903754`.
Passano **1.316 test runtime**, con 31 esclusi, sia nella build ordinaria sia
con ASan/UBSan. Sono gli stessi contratti eseguiti due volte; dodici test sono
nuovi. I sanitizzatori riguardano l'eseguibile dei test con gioco stub, non
l'intero gioco ricompilato. LeakSanitizer è disabilitato e il supporto alle
stack `makecontext/swapcontext` resta incompleto.

| Regressione | Esito |
| --- | --- |
| Codec al molo | Apertura/chiusura native e donor; replay esatto di 147 frame |
| Cambio disco | s12c → s13a, movimento controllato, un cambio e una validazione originali; replay esatto di 504 frame |
| Caricamento retail CD2 | s18a/277 controllabile e accovacciamento/rialzata; quattro intervalli loader da 4–6 VBlank, s18a in 6 |
| Ingresso sintetico s08b/6137 | 388 fotogrammi, 77.600 macroblocchi, 487 settori XA e 1.145.424 frame PCM stereo consumati; endpoint concorde col donor storico |
| Replay storico NSV10 | 1.460 frame senza desincronizzazione RAM/continuazione |

Tutte le esecuzioni native terminano senza errori di gioco o ricorso
all'interprete. Sono sette esecuzioni native includendo i replay, non sette
segmenti nuovi di campagna. Nessun hash delle carte personali o del savestate
personale è cambiato. I conteggi video/audio non certificano l'immagine
visualizzata o la sincronizzazione audiovisiva; i tempi loader sono misure
headless in VBlank, non latenze del monitor.

Il [rapporto con hash e riferimenti alle prove](mgs-mmio-evidence.json)
permette di risalire a sorgenti, binari, log e input. Analizzatore,
ricompilatore e suite Python non sono stati rieseguiti: i loro sorgenti non
sono cambiati in questa verifica.

## Matrici riproducibili

`engine/ps1Test/runtime/test_memory_mmio.cpp` attraversa tre alias
KUSEG/KSEG0/KSEG1, ampiezze 8/16/32 bit e tutte le posizioni allineate previste
da ciascun contratto. Comprende tre timer, sette canali DMA, quattro indici
CD e due porte controller. I test in `test_psyq_pad.cpp` controllano inoltre
scollegamento/ricollegamento e coerenza delle interrogazioni SDK con il TAS.
Il rapporto rosso originale conserva dieci fallimenti su undici test nuovi;
la matrice di lettura DMA passava già e protegge il cambiamento di percorso.

```bash
cmake --build build --target ps1Runtime ps1Runtime_tests --parallel 4
build/ps1Test/ps1Runtime_tests --gtest_filter='MemoryMMIO.*:PsyqPadTest.*'
```

Evidenze locali, con log e JSON dei test, sotto
`data/validation/completion/mmio-20261006/`. Il rapporto consolidato registra
hash dei sorgenti, binari e prove di regressione; i conteggi dei test non
sono conteggi di segmenti di campagna completati.

## Riferimenti e limiti

Il confronto del trasporto usa `src/core/bus.cpp` e `src/core/pad.cpp` di
SwanStation alla revisione registrata in `donor/source.json`, in particolare
`DoAccessTimers`, `DoDMAAccess`, `DoCDROMAccess` e `DoPadAccess`. È un confronto
del codice del donor, non una nuova esecuzione differenziale di tutti i
registri. Il confronto di gioco con SwanStation è registrato separatamente.

Le [misure PSX-SPX sulle scritture parziali](https://psx-spx.consoledev.net/ps1/system/partialwordwrites/)
documentano differenze fra bus e il ruolo dell'intero registro MIPS sorgente.
L'API del runtime tronca già `SB`/`SH` a 8/16 bit: questa revisione mantiene il
precedente contratto DMA a maschera e ne corregge l'atomicità. Non sostiene
quindi di riprodurre integralmente le scritture parziali del silicio.
Ricostruire anche quelle richiede propagare il valore sorgente attraverso
ricompilatore, interprete e accessi MMIO, con una verifica dedicata.

Sono ancora aperti: temporizzazione/FIFO/ACK del SIO, protocollo seriale
completo delle memory card, pacchetto DualShock2, accessi non allineati e
open-bus, ampiezze non verificate delle altre periferiche. La memory card
usata dal gioco passa dai percorsi BIOS/HLE già verificati; instradare
JOY_DATA non certifica il protocollo seriale della carta. Le modifiche non
intervengono negli shader o nella composizione Vulkan e non costituiscono
una nuova prova visiva.
