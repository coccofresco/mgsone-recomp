# Effetti dello schermo nella presentazione Vulkan

La presentazione sostituisce gli attori riconosciuti con passaggi continui
sull'immagine Vulkan RGB8. La risoluzione predefinita rimane 3840×2160, 16:9;
il framebuffer software, i comandi GPU e la logica del gioco rimangono nativi.
Questi effetti sono attivi per impostazione predefinita nel profilo MGS USA.

## Percorsi implementati

| Origine | Resa Vulkan | Dati conservati dal gioco |
| --- | --- | --- |
| `wt_view.c` | Rifrazione bidimensionale continua, già presente | Attivazione, fase, area subacquea |
| `scn_mask.c`, visore notturno | Risposta verde del sensore, guadagno, vignettatura morbida e rumore fine; 113 primitive diventano un passaggio | Attore attivo, modalità e posizione nell'ordine di disegno |
| `scn_mask.c`, visore termico | Ottica rossa continua senza le 112 righe scure | Identità termiche e palette degli oggetti restano quelle del gioco |
| `fadeio.c` | Dissolvenza verso nero o bianco che conserva i rapporti fra i canali, senza saturarli uno alla volta | Intensità del TILE effettivamente disegnato, direzione, durata, pausa |
| `blurpure.c` | Accumulo temporale dell'immagine moderna, con cinque campioni per ammorbidire la scia | Peso della scia, doppio buffer, ritaglio cinematografico 320×160 |
| `gas_efct.c` | Alone ellittico con gradiente continuo, senza i sedici settori poligonali | Raggio, proporzioni, colori animati interni/esterni, composizione additiva/sottrattiva |

La grana del sensore è un pattern stabile calcolato per pixel Vulkan: non
avanza con l'orologio host durante pausa o caricamento di un salvataggio.
La sfocatura usa la superficie moderna del fotogramma precedente; non copia
la versione RGB555 a 320×224. Non introduce ancora interpolazione temporale 2×.

## Collegamento alla versione USA

Riferimento logico: FoxdieTeam/mgs_reversing, revisione locale
`217509eae471629b1a53633b816c61adfef9ed5c`, file `libgv/actor.c`,
`takabe/scn_mask.c`, `takabe/fadeio.c`, `okajima/blurpure.c`,
`takabe/gas_efct.c`. Le strutture sono confrontate con la RAM USA dell'intro,
non con gli indirizzi delle funzioni Integral.

- `ActorList` USA parte da `0x800af390`: nove liste da 68 byte, con sentinelle
  `GV_ACT` da 32 byte. Ogni visita verifica collegamenti all'indietro, termine
  corretto e assenza di cicli prima di pubblicare le appartenenze.
- Visore: livello ASSIST, puntatore primitive a `Work+32`, due TILE filtro a
  `prims+16`, 224 TILE di scansione a `prims+64`, dimensione totale 3648 byte.
  Si verificano anche opcodes, colori, dimensioni e posizione di ogni riga.
- Dissolvenza: PREV2, puntatore a `Work+36`, modo a `Work+52`. L'intensità si
  legge dal pacchetto disegnato, evitando lo scarto dell'attore che ha già
  incrementato il contatore per il fotogramma seguente.
- Accumulo: AFTER2, puntatore a `Work+32`, SPRT a `prims+112` e `prims+152`.
  Il `DR_STP` USA occupa 12 byte, diversamente dagli otto di `DR_TPAGE`.
- Alone: PREV2, puntatore a `Work+40`, stato a `Work+64`, raggio a `Work+92`,
  proporzione a `Work+100`; due gruppi di sedici `POLY_G4` da 36 byte.

L'appartenenza richiede l'indirizzo esatto del comando GP0 nella struttura
dell'attore vivo. Una forma o un colore simile in un menu non basta. Il
presenter sostituisce un gruppo solo se tutti i suoi elementi sono presenti
nella stessa submission e nello stesso buffer, senza duplicati o scritture
interposte. Se la prova manca conserva l'intero percorso precedente. I dati
pubblicati sono copie dei parametri: nessun puntatore RAM attraversa la coda.

L'immagine sorgente è una copia ordinata degli attachment Vulkan, con il
tracking delle scritture già usato dalla rifrazione. Il risultato può essere
nero visibile; non eredita il significato PSX di texel zero trasparente.
Le sostituzioni non modificano la VRAM, le dipendenze native delle texture,
gli upload originali, l'audio o i salvataggi.

## Verifica e riproduzione

Test GPU specifici verificano un bordo spostato di un quarto di pixel
originale attraverso il visore, i valori RGB8 delle dissolvenze a intensità
0/128/255, il piano mask e l'identità di tutta la VRAM software rispetto
all'esecuzione degli stessi comandi senza presentazione moderna. Altri test
coprono liste corrotte, primitive disallineate, reset delle appartenenze,
gruppi incompleti/duplicati/intercalati e dipendenze fra i due buffer.

La verifica del 15 settembre 2026 passa **271 test in 18 suite**, con Vulkan,
cache originale e istruzioni USA come oracolo abilitati. L'avvio completo
di 15.600 VBlank arriva al menu: 13 acquisizioni 3840×2160, **98.140 primitive
sostituite da 3.301 passaggi**, tutte e sei le nuove varianti osservate,
nessun gruppo incompleto e nessun rifiuto dell'esecuzione Vulkan.
L'unica inizializzazione dalla superficie nativa è quella dell'avvio.

Il confronto separato dallo stesso salvataggio, con 600 VBlank dopo il
caricamento e cadenza NTSC normale, misura **8,058 ms medi GPU** con gli effetti
moderni e **8,116 ms** con i precedenti, su RTX 4060 (602 submission ciascuno).
È sostanziale parità di costo nel tratto dei due visori, con quattro letture
diagnostiche per esecuzione; non misura tutti gli effetti o tutto il gioco.
Le acquisizioni accelerate dell'intro servono invece alla copertura.

Le evidenze della sequenza USA e i contatori sono registrati in
[mgs-screen-effects-evidence.json](mgs-screen-effects-evidence.json).
Le acquisizioni e le istantanee diagnostiche sono locali, sotto `data/native`.

```bash
scripts/run_native.sh

# Confronto diagnostico: primitive degli attori precedenti, mantenendo
# le texture dello schermo moderne e la rifrazione subacquea.
PS1_MGS_NATIVE_SCREEN_EFFECTS=1 scripts/run_native.sh
```

## Copertura ancora da estendere

Non è una certificazione di tutti gli effetti di ogni livello. `blur.c`
(zoom/deformazione a quattro quadranti), `focus.c` (selezione della profondità
con STP), `mosaic.c`, distorsioni locali di superfici/materiali e gli effetti
specifici dei boss richiedono adattatori e scene di verifica propri.
`cinema.c` disegna già bande piene in Vulkan; scuotimento della camera e
particelle appartengono invece ai percorsi della scena. Texture, video e
caratteri bitmap conservano il dettaglio dei dati originali.
La verifica termica riguarda l'ottica nelle scene provate, non tutti i
materiali termici e i relativi cambi di palette di ogni mappa.
