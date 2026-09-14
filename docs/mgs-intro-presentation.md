# Intro e rifrazione subacquea in Vulkan

Branch `presentation/intro`, derivato da `decomp/native-core`.
L'acqua usa una passata continua sull'immagine moderna: non riproduce la
suddivisione in strisce della PS1. La scena mantiene il target 3840×2160,
il rapporto 16:9 fisso e la geometria originale residente. La GPU software,
la VRAM, le letture del gioco e i savestate mantengono il formato nativo.

## Effetto moderno

La sorgente è l'attore
[`wt_view.c`](https://github.com/FoxdieTeam/mgs_reversing/blob/217509eae471629b1a53633b816c61adfef9ed5c/source/takabe/wt_view.c).
Il bridge percorre la lista USA `GV_ACTOR_PREV2`, verifica collegamenti,
nome dell'attore, allocazione e appartenenza dei pacchetti alle sue quattro
matrici SPRT. Non identifica l'acqua dal colore dell'immagine o dal numero
di righe. La fase attraversa la coda di presentazione insieme al pacchetto.

Le strisce di uno stesso attore, buffer e submission diventano un unico
quad. Lo shader `games/mgs_screen_texture.glsl` applica una deviazione
continua verticale e una componente orizzontale più lieve, attenuate vicino
ai bordi. Entrambe variano con X e Y: le creste si incurvano lungo l'immagine,
invece di spostare uniformemente ogni riga. La fase deriva dall'attore;
pause e riprese da savestate non introducono un orologio host indipendente.
Il periodo orizzontale è doppio
rispetto a quello verticale, senza discontinuità al riavvolgimento della fase.
Colore, cadenza e intensità si riferiscono all'effetto originale, mentre il
campo di deformazione e il filtraggio sono una scelta di presentazione moderna.

Colore RGB8 e maschera vengono letti dalla superficie Vulkan, conservando
coordinate frazionarie e dettaglio della geometria. Il campionamento non
passa attraverso la copia RGB555 a 320×224. La maschera resta separata
dall'alpha, compreso il nero visibile con bit STP. Il testo disegnato dopo
l'effetto conserva la propria posizione nell'ordine di composizione.

Anche gli altri sprite e rettangoli MGS sono ora operazioni geometriche
nel replay, anziché righe di pixel già composte dalla GPU software. Questo
rimuove la sovrascrittura del dettaglio moderno e il costo di migliaia di
piccole operazioni. Le texture framebuffer a 16 bit possono leggere la
superficie moderna; il tracciamento delle scritture impone una nuova copia
quando una passata legge un risultato precedente. Le texture indicizzate
continuano a usare materiali originali e palette vive.

Il blending Vulkan ereditato da Crash 3 è abilitato anche per MGS sulle
GPU con `independentBlend`. Mantiene la selezione STP, le quattro modalità
ABR e la maschera separata. I casi che leggono la maschera destinazione
conservano il percorso con snapshot. `PS1_MGS_EXACT_TRANSPARENCY=1` permette
il confronto con il blending intero tramite snapshot; l'approssimazione
ABR0 del percorso veloce è la stessa già impiegata da Crash 3.

## Correzioni necessarie all'intro

- `GPUSTAT` espone la disponibilità di `GPUREAD`; DMA2 dispositivo→RAM
  consuma lo stesso cursore della CPU. Prima, `StoreImage2` andava in timeout
  e le palette dei visori risultavano corrotte. Il percorso CPU, il completamento
  DMA e l'invalidazione della provenienza RAM sono verificati insieme.
- La matrice Q12 `DG_OBJ.screen` verifica la proiezione nativa quando è
  disponibile. Non sostituisce la matrice in virgola mobile della presentazione.
- I materiali opachi recuperati mantengono il selettore ABR della texture page.
  Il precedente disaccordo fra campi decodificati e tpage faceva rifiutare
  interi fotogrammi, mostrando la versione nativa ingrandita.
- Lo scorrimento delle texture copia verso l'alto rettangoli sovrapposti
  (`64,398 → 64,391`, `32×56`, e l'altra pagina a Y270→263). Il controllo
  delle dipendenze ora prova l'ordine delle righe, invece di rifiutare ogni
  sovrapposizione verticale. Le copie che rileggono una riga già sovrascritta
  restano rifiutate. L'ordine è confrontabile con `GPU_SW_Backend::CopyVRAM`
  del donor SwanStation locale.

## Verifica

I 209 test pertinenti passano con integrazione Vulkan, cache USA originale
e oracolo delle istruzioni USA abilitati. Un test GPU disegna una figura
spostata di un quarto di pixel PSX, la rilegge come texture dello schermo e
verifica gli otto campioni RGB8 risultanti: quattro campioni distinti per
pixel nativo sopravvivono. Non basta controllare la dimensione del file.
Altri test verificano appartenenza all'attore, aggregazione delle strisce,
dipendenze fra passate, UV oltre 255, sprite specchiati che attraversano
lo zero e identità della VRAM software.

Nel confronto dalla stessa istantanea subacquea, per 600 submission con
cadenza VBlank normale, la RTX 4060 passa da **39,961 ms a 9,348 ms medi GPU**.
Sono tempi GPU del tratto provato, con letture diagnostiche abilitate, non
una promessa di framerate per l'intero gioco. Le 67.580 strisce di quel
tratto diventano 300 passate moderne, una per fotogramma del gioco.

L'avvio completo di 15.600 VBlank arriva al menu iniziale: 13 acquisizioni
3840×2160, un caricamento iniziale della superficie nativa e nessun successivo
rifiuto del replay moderno. Questa esecuzione usa VBlank accelerati per
la copertura della sequenza. Dopo il ritocco finale del campo bidimensionale,
il tratto subacqueo è stato ripetuto a cadenza normale per il confronto sopra,
con cinque nuove acquisizioni e nessun rifiuto del replay.

Acquisizioni, contatori, hash e limiti della verifica completa sono in
[mgs-intro-presentation-evidence.json](mgs-intro-presentation-evidence.json).
Le immagini e le istantanee diagnostiche restano sotto `data/native`, fuori Git.
Il salvataggio utente non viene sovrascritto.

```bash
scripts/run_native.sh

# Verifica GPU senza aprire una finestra visibile; mantiene il renderer Vulkan.
PS1_VULKAN_HIDDEN_WINDOW=1 PS1_PRESENT_WINDOWED=1 \
PS1_EXIT_VBLANK=15600 PS1_METRICS=data/native/intro-check/metrics.json \
scripts/run_native.sh
```

Geometria ed effetti dello schermo sono rasterizzati ad alta risoluzione.
Le texture e i caratteri bitmap mantengono il dettaglio degli elementi
originali: questa modifica non produce nuove texture HD né font vettoriali.
La migrazione completa della logica al C decompilato resta un lavoro distinto.
