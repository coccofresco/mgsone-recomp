# Bootstrap — 10 settembre 2026

## Riferimento esterno

https://github.com/Alexbeav/metal-gear-solid-recomp
Commit consultato: `ea9b7bc87de1354d032fe3ffd321ed17a0fa4307`.
Checkout consultabile localmente in `.local/reference-mgs` (non tracciato).

Il progetto usa PSXRecomp e recomp-ui, distinti dal PS1Recomp locale di Crash.
La configurazione riguarda SLES-01370/SLES-11370, usa seed Ghidra e BIOS retail
PAL. È utile per confrontare struttura del titolo e preparazione dei dischi;
seed, API del runtime e configurazioni non sono direttamente intercambiabili.
Non sono stati importati sorgenti o seed da questo riferimento.
Il README distingue verifiche di build e accettazione del gameplay.

## Risultati locali

- Configurazione CMake Release riuscita, con runtime in modalità stub.
- Compilati ps1Analyzer, ps1Recomp e le rispettive suite di test.
- Analizzatore: 121 test passati.
- Ricompilatore: 128 test passati, 1 saltato perché richiede un caso Crash.
- Entrambi i CD letti; eseguibili estratti e ricette iniziali generate.
- Generazione C++ riuscita per entrambi gli eseguibili: 114 funzioni ricompilate,
  129 voci di dispatch per CD. Traduzioni locali non ancora compilate o eseguite.
- Seriali rilevati dai percorsi di boot: SLES-01508 / SLES-11508.
- Entrambi gli header: ingresso `0x80066A98`, caricamento `0x80010000`,
  dimensione testo 362496 byte, stack `0x801FFFF0`.
- Gli hash degli eseguibili differiscono: non assumere codice identico fra CD.

Log completi in `.local/` e `data/`, esclusi da Git. Non sono stati verificati
build completa del runtime, avvio del gioco, overlay, resa grafica o gameplay.

## Confine di riuso

Il motore è una copia dei file tracciati selezionati, con il contenuto effettivo
della working tree, non un collegamento al checkout Crash. Gli hash documentano
anche i file modificati rispetto al commit di provenienza. Il diff originale è
conservato localmente in `.local/crash3-working-tree.patch`.
Sono esclusi traduzioni retail generate, build, prove grafiche e dati del gioco.
Il codice specifico Crash resta presente dove il runtime e i test lo richiedono;
le sue assunzioni non costituiscono un profilo MGS. Il primo obiettivo tecnico è
una baseline fedele e misurabile sul titolo italiano.
