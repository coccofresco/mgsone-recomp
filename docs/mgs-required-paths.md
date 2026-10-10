# Percorsi obbligati: verifica dei passaggi di progressione

Il [seguito del 7 ottobre](mgs-alpha-gameplay-soak.md) copre ora le fasi tarde
di Mantis fino alle grotte, il ritorno dal tetto dell'Hind con ingresso retail
valido e la fuga in jeep fino al passaggio originale al finale. Le soglie di
danno preparate rimangono esplicite; non sono combattimenti completi certificati.
La sezione del 6 ottobre qui sotto conserva le evidenze e i problemi storici.

Aggiornamento del 6 ottobre 2026. La priorità è verificare i passaggi che
permettono di proseguire la campagna: condizioni di ingresso, evento originale,
oggetto/flag ottenuto e uscita. I caricamenti isolati e i test del runtime
restano utili, ma non certificano questi passaggi.

Le [prime evidenze con impronte dei file](mgs-required-paths-evidence.json)
distinguono prove riuscite, preparazioni sintetiche e problemi aperti. Ogni
risultato conserva l'identità del runtime, dei checkpoint e del donor usati.
Sono prove headless della logica; non verificano la resa Vulkan, l'audio
percepito o il comportamento a velocità reale.
La [continuazione e il fix delle callback](mgs-required-paths-next-evidence.json)
conservano anche gli insuccessi precedenti e distinguono la build `d5c5168b…`
dalla nuova `9916ff0d…`.

## Carroarmato

La procedura originale USA `s05a/ea54:a4ad` inizializza insieme storia, mappa,
posizione, contatori e salute dei mitraglieri. Il nuovo preparatore importa
tutte le assegnazioni costanti della procedura, rifiutando istruzioni dinamiche
o effetti non modellati. Prima cambiavamo solo area e storia: gli altri campi
potevano restare quelli del salvataggio di partenza.

Le quattro prove accettate esercitano:

1. Attivazione del primo mitragliere, con Snake avvicinato da una fixture.
2. Morte del primo mitragliere dopo danno sintetico nel suo `TARGET`, seguita
   dalla creazione del secondo con la salute originale.
3. Morte del secondo, distruzione del carroarmato, callback originale,
   tessera livello 3 e caricamento di `s06a`, storia 69.
4. Fine dell'ingresso automatico nell'edificio, ritorno dei comandi e salute
   massima/vitale a 384, come prescritto dalla procedura `s06a/ea54:8a53`.

Per il secondo mitragliere viene predisposta la salute terminale dopo il
calcolo del danno: la sua invulnerabilità iniziale assorbiva un danno inserito
troppo presto. Non viene forzato alcun esito, callback o avanzamento della
storia. Questo verifica la catena successiva al danno; lancio delle granate,
collisioni e combattimento completo rimangono da provare. L'avvicinamento
predisposto non certifica il superamento del campo minato.

Il controllo successivo `tank-control-verified` distingue inoltre
l'esistenza dell'actor Snake dal termine dell'animazione automatica e dal
rilascio dei blocchi agli input. Passano entrambi i motori e il replay nativo.
La prima versione della ricetta richiedeva erroneamente salute massima
invariata: il codice originale la aumenta proprio qui. Il contratto corretto
richiede l'aumento previsto, senza scrivere salute o flag nel gioco.

## Prigione e visita di Otacon

Il percorso parte dall'esito già verificato della resa alla tortura, con
ascendenza sintetica. Il codice `chara/torture/otacom.c` attiva il dialogo
quando Snake è abbastanza vicino; gli script USA assegnano ketchup e tessera.
Sono riusciti, su entrambi i motori e con replay nativo:

- Avvicinamento con i comandi originali e attivazione della scena: variabile
  globale `0x66` da 1 a 2.
- Consegna originale di ketchup (1) e tessera livello 6, uscita di Otacon e
  ritorno ai comandi, senza nuove scritture nella RAM.

Il vecchio obiettivo `0x66 >= 4` apparteneva a una successiva variante di
soccorso. Anche aspettare una coordinata dopo l'avvio della scena era errato:
il gioco aveva già tolto i comandi. Le ricette ora attendono l'evento pertinente.
I due risultati riusciti conservano i precedenti timeout nella propria
`diagnostic_ancestry`; non certificano retroattivamente le ricette fallite.

La continuazione `required-paths-next-20261006` verifica ora anche la fuga
con il ketchup. Cinque contratti consecutivi passano su entrambi i motori,
ciascuno con replay nativo esatto:

1. Selezione del ketchup, posizione prona e uso dentro la zona originale:
   l'oggetto viene consumato e si attiva lo stato di finta morte.
2. Johnny nota Snake, apre la cella ed entra: globale `0x66 = 3` e apertura
   della porta osservata nella variabile originale `0x0243`.
3. Snake si rialza e stordisce Johnny con i pugni. La callback originale
   imposta il bit di guardia incosciente `0x0267`; il danno subito da Snake
   porta la vita da 576 a 512 in entrambi i motori.
4. Percorso attraverso i corridoi, tessera livello 6 **equipaggiata** e
   recupero dell'equipaggiamento. La procedura originale `791e` restituisce
   le armi confiscate e imposta il flag di recupero `0x004e`.
5. Fine dell'animazione di recupero, porta d'uscita e caricamento originale
   di `s03a`, storia **163**, con i comandi disponibili, vita 512, armi e
   inventario concordi fra i motori.

Tutti e cinque i contratti usano solo input: nessuna nuova scrittura di RAM,
nessun teletrasporto o avanzamento forzato. Resta l'ascendenza sintetica della
cattura iniziale, con i timeout storici conservati. Il primo tentativo senza
tessera equipaggiata si fermava correttamente alla porta: è un errore della
ricetta, conservato separatamente, non un difetto del gioco.

Restano da verificare gli altri metodi di evasione, il ramo in cui Snake
resiste alla tortura e il raggiungimento continuo della cattura. La fuga con
ketchup è verificata dal checkpoint della visita, non dall'inizio della partita.

## Hind: corretta la preparazione della mappa

La fixture di salute terminale raggiunge la morte dell'Hind e la procedura
originale `s11h/ea54:2324`, che porta direttamente alla storia **188**.
Aspettare 187 era una condizione errata del test.

Con la vecchia preparazione, il filmato successivo si arrestava nello stesso stato su native e donor:
`pause_level=8` (`GV_PAUSE_READERROR`), storia 188, Hind distrutto. La ripetizione
senza input durante il filmato riproduce il blocco: non dipende dal tentativo
di saltarlo. Nel dump sono presenti pacchetti DMO accodati, ma manca l'actor
`demothrd.c` che dovrebbe consumarli; il tick dello stream resta a 129.
Le risorse modello richieste dalla definizione DMO risultano nella cache.

La causa individuata è la mappa ereditata dal salvataggio della torre:
`0xf1f2`, mentre l'ingresso originale sul tetto usa **`0x7df9`**. L'operando
proviene dal comando `load "s11h" -m 0x7df9` nella procedura retail
`s11c/ea54:f9db`. Il preparatore ora lo importa verificando destinazione,
unicità e valore letterale; rifiuta operandi dinamici o ambigui. Questa
proiezione del campo mappa non pretende di eseguire l'intera procedura d'ingresso.

Con la mappa corretta, lo stesso runtime `d5c5168b…` e la stessa ricetta di
danno terminale superano il filmato e arrivano a **storia 190**, vita 640,
nessuna pausa e comandi disponibili: native 4778 frame, SwanStation 4739,
replay nativo esatto. Il risultato `hind-map-aftermath` corregge quindi un
falso blocco introdotto dalla fixture; non viene presentato come fix del runtime.

Il danno terminale resta sintetico e questa ricetta contiene input di salto
del filmato. La prova successiva `hind-map-unskipped`, sulla nuova build,
lascia terminare tutto il filmato e preme Cross solo durante il Codec:
native **4867 frame**, donor **4858**, replay nativo esatto. L'esito è ancora
storia 190, vita 640 e comandi disponibili. Il combattimento completo con
Stinger, il percorso precedente e l'uscita dal tetto richiedono contratti distinti.

L'ulteriore prova dell'uscita attraversa la porta con tessera e raggiunge le
scale, ma resta sul tetto in entrambi i motori. Gli script richiedono direzione,
una pressione di Cerchio (`ntrap -b 0`) e **`0x02e7` bit 2 disattivato**;
il checkpoint sintetico eredita quest'ultimo già attivo dal salvataggio della
torre. È presente prima della prova e rimane attivo dopo. Il passaggio non è
quindi certificato. La ricetta ora controlla questa precondizione prima di
eseguire input: occorre ricostruire la storia d'ingresso originale, senza
azzerare il flag per forzare l'uscita. I due tentativi restano nei diagnostici.

## Mantis: crash della recomp individuato nel primo scontro con Meryl

L'ingresso di prova parte ora dalla storia 120 e lascia alle scene originali
l'impostazione della posizione di Snake e il passaggio alla storia 123.
Il precedente caricamento isolato dalla storia 124 ereditava una posizione
inadatta dal bagno e non costituiva un ingresso valido.

Con questo ingresso, una ricetta di soli comandi raggiunge Meryl e la
stordisce senza armi. SwanStation completa la scena seguente e arriva al
combattimento con Mantis, storia 125. La recomp precedente si arresta invece
chiamando `0x800DA870`: la callback `Die` di `padmng.c` non era registrata.

Il generatore riconosceva una funzione referenziata solo dopo un ritorno
`JR RA / NOP`. Qui il ritorno precedente esegue uno store nel delay slot:
è codice valido del compilatore originale. Il riconoscimento ora ammette
queste istruzioni utili, mantenendo il requisito di un riferimento esplicito
alla funzione e rifiutando trasferimenti di controllo nel delay slot.
L'audit dei 73 overlay trova 13 ingressi prima mancanti in 10 overlay e zero
ingressi mancanti dopo la rigenerazione. Questo numero misura la copertura
statica: il crash di Mantis è quello riprodotto in esecuzione.

Sulla nuova build **`9916ff0d…`**, dal medesimo checkpoint e con la medesima
ricetta, il passaggio riesce: native **3016 frame**, donor **3058**, replay
nativo esatto. Snake conserva vita 414, l'actor di Mantis è presente e la
storia è 125 con i comandi disponibili. Non viene inserita una callback vuota:
viene ricompilata ed eseguita la funzione originale che libera il gestore del pad.
Restano aperti le fasi del boss, la seconda possessione di Meryl, i percorsi
controller/statue e l'apertura del passaggio finale.

## Metodo e priorità successive

`scripts/mgs_required_paths.py` prepara carte isolate. Le scritture riportano
fonte, larghezza e limiti; le ricette conservano condizioni iniziali e finali.
`scripts/mgs_situations.py continue --resume-timeout` è consentito solo per
timeout della route senza errori, divergenze o invarianti alterate, con hash,
RAM e checkpoint verificati. Crash, timeout del processo e artefatti modificati
restano rifiutati. La ripresa è esplicita e non cancella l'insuccesso precedente.

La prossima priorità è completare le fasi di Mantis e l'uscita dall'Hind,
poi le lacune dei passaggi obbligati già censiti sui due dischi. Un boss
caricato, un filmato terminato e una vittoria completa restano prove distinte.

Le carte e il savestate personali restano invariati. La continuità dell'intera
campagna, i combattimenti completi e tutte le varianti di difficoltà rimangono
non certificati.

Questa iterazione aggiunge **sette contratti riusciti**: cinque per la fuga,
uno per Meryl/ingresso Mantis e uno per il seguito integrale dell'Hind. Il
manifesto complessivo contiene 143 contratti passati e 29 capitoli con copertura
parziale, **nessun capitolo interamente certificato**. Passano 151 test Python
e la ripetizione dei 1325 test runtime, con 31 esclusi; non è stata ripetuta
la batteria strumentata ASan/UBSan in questa iterazione.

```bash
# Generazione riproducibile dell'ingresso; usare una cartella nuova.
python3 scripts/mgs_required_paths.py tank data/validation/tank-next

# Ripetizione di un contratto dal checkpoint con cui è stato verificato.
python3 scripts/mgs_situations.py continue \
  data/validation/completion/required-paths-20261006/tank-gunner1/result.json \
  data/validation/tank-victory-next \
  --recipe scripts/fixtures/completion/tank-second-gunner-hit.json \
  --donor-runner .local/libretro_completion --verify --period-us 1000

# Riproduzione del crash corretto: stesso checkpoint e stessi input.
python3 scripts/mgs_situations.py continue \
  data/validation/completion/required-paths-next-20261006/mantis-meryl-ready/result.json \
  data/validation/mantis-callback-next \
  --recipe scripts/fixtures/completion/mantis-first-meryl.json \
  --donor-runner .local/libretro_completion --verify --period-us 1000
```
