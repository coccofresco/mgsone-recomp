# Effetti dello schermo nella presentazione Vulkan

La presentazione sostituisce gli attori riconosciuti con passaggi continui
sull'immagine Vulkan RGB8. La risoluzione predefinita rimane 3840×2160, 16:9;
il framebuffer software, i comandi GPU e la logica del gioco rimangono nativi.
Questi effetti sono attivi per impostazione predefinita nel profilo MGS USA.

## Percorsi implementati

| Origine | Resa Vulkan | Dati conservati dal gioco |
| --- | --- | --- |
| `wt_view.c` | Correnti bidimensionali a più scale, rifrazione con dispersione cromatica, caustiche animate e assorbimento azzurro | Attivazione, fase completa, area subacquea |
| `scn_mask.c`, visore notturno | Amplificazione del segnale, fosforo verde con alte luci gialle, diffusione luminosa a due scale, grana grossa animata e vignettatura; 113 primitive diventano un passaggio | Attore attivo, modalità e posizione nell'ordine di disegno |
| `scn_mask.c`, visore termico | Palette dal blu freddo al magenta, giallo e bianco; emissione dei corpi anche nelle zone buie | Modalità attiva e materiali IR, con classificazione artistica dei corpi nelle cinematiche |
| `fadeio.c` | Dissolvenza verso nero o bianco che conserva i rapporti fra i canali, senza saturarli uno alla volta | Intensità del TILE effettivamente disegnato, direzione, durata, pausa |
| `blurpure.c` | Scie direzionali con decadimento esponenziale, persistenza sensibile al movimento e lieve separazione cromatica delle code | Peso della scia, doppio buffer, ritaglio cinematografico 320×160 |
| `gas_efct.c` | Alone ellittico con gradiente continuo, senza i sedici settori poligonali | Raggio, proporzioni, colori animati interni/esterni, composizione additiva/sottrattiva |

La revisione espressiva interpreta liberamente l'ottica, mantenendo attivazione
ed evoluzione degli attori originali. La grana del notturno ha celle di circa
sei pixel UHD e un'ampiezza oltre dieci volte superiore alla prima versione,
più aggregati morbidi a scala maggiore. Avanza con `GV_Time`, non con un timer
host. Durante la pausa viene scelto un seme fisso: può cambiare pattern quando
si entra in pausa, poi resta fermo. Ricaricare lo stesso istante riproduce la
stessa grana. La risoluzione dell'effetto segue l'immagine moderna.

L'acqua combina correnti e vortici lenti, tre campi di onde, tre campioni RGB
per la dispersione e caustiche mobili. Usa il contatore completo di `wt_view`,
eliminando il salto periodico causato dal precedente mascheramento a 63.
È un trattamento ottico della scena: non simula il volume dell'acqua.

Le scie leggono l'immagine moderna del buffer precedente. Una stima limitata
del movimento dai gradienti d'immagine orienta sei campioni con decadimento,
conservando il centro nitido quando la scena è ferma. I riflessi persistono
nelle zone scure e la coda vira leggermente verso l'azzurro. Non si tratta
ancora di vettori di movimento derivati dalla geometria né di interpolazione
2×. L'intensità originale resta il comando della persistenza.

Il termico rimappa la risposta dei materiali in falsi colori. Gli oggetti con
`DG_FLAG_IRTEXTURE` ricevono un minimo emissivo quando `STATE_THERMG` è attivo.
Per le cinematiche si aggiunge un profilo caldo ai rig con almeno dieci ossa,
rotazioni valide e una gerarchia KMD collegata a un'unica radice. Il controllo
dei genitori è necessario: il sottomarino dell'intro ha 13 parti con rotazioni,
ma tutte indipendenti; Snake ha 16 ossa collegate. Il solo numero di parti
scaldava erroneamente anche lo scenario. Questa classificazione è una scelta
artistica, non una misura fisica della temperatura o un riconoscimento di ogni
personaggio del gioco. I profili di altri rig possono richiedere affinamento.

## Collegamento alla versione USA

Riferimento logico: FoxdieTeam/mgs_reversing, revisione locale
`217509eae471629b1a53633b816c61adfef9ed5c`, file `libgv/actor.c`,
`takabe/scn_mask.c`, `takabe/fadeio.c`, `okajima/blurpure.c`,
`takabe/gas_efct.c`, `libdg/bound.c`, `takabe/goggleir.c`. Le strutture sono confrontate con la RAM USA dell'intro,
non con gli indirizzi delle funzioni Integral.

- `ActorList` USA parte da `0x800af390`: nove liste da 68 byte, con sentinelle
  `GV_ACT` da 32 byte. Ogni visita verifica collegamenti all'indietro, termine
  corretto e assenza di cicli prima di pubblicare le appartenenze.
- Tempo USA: `GV_Time` a `0x800ada48`, `GV_PauseLevel` a `0x800ae0a0`;
  `GM_GameStatus` a `0x800adb3c`, bit termico `0x8`, bit demo `0x80000000`.
  Le istruzioni dell'eseguibile USA verificano gli indirizzi del tempo e dello
  stato; le semantiche dei flag provengono dalla decompilazione.
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

Le copie dello schermo che superano un bordo usano il ritaglio della viewport.
Nell'intro due SPRT da 160 pixel iniziano a X=-2 e X=158: la prima metà veniva
respinta perché parzialmente fuori schermo, mescolando un'immagine RGB555 con
la metà moderna. Ora entrambe campionano la stessa scena moderna; la dipendenza
sorgente conserva l'intero intervallo UV e quella di destinazione considera
solo i pixel effettivamente disegnati. Un test Vulkan verifica il bordo
subpixel attraverso una copia che inizia a X=-2.

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
coprono grana visibile su fondo uniforme, animazione/pausa/riavvolgimento del
sensore, emissione subordinata alla modalità termica, esclusione degli scenari
con molte radici, separazione dai modi delle geometrie procedurali, liste corrotte, primitive disallineate, reset delle appartenenze,
gruppi incompleti/duplicati/intercalati e dipendenze fra i due buffer.

La prima implementazione degli attori (`5971c26`) aveva superato 271 test e
l'intro completa. Il relativo confronto GPU di 8,058 / 8,116 ms riguarda quella
versione, non i nuovi shader più espressivi; le evidenze storiche rimangono in
[mgs-screen-effects-evidence.json](mgs-screen-effects-evidence.json).

La revisione espressiva passa **277 test in 18 suite**. L'avvio completo di
15.600 VBlank raggiunge il menu, con 13 acquisizioni 3840×2160 e 3.291
passaggi degli effetti. Nessun gruppo incompleto, materiale mancante o
rifiuto del percorso Vulkan; una sola inizializzazione nativa all'avvio.

Nei tre tratti a cadenza NTSC normale si misurano mediamente **9,820 ms GPU**
per l'acqua, **7,109 ms** per i visori e **4,222 ms** per le scie/transizioni,
su RTX 4060. Sono rispettivamente 602, 602 e 482 submission, incluse
inizializzazione/caricamento e letture diagnostiche. Sono medie di ciascun
tratto, non tempi massimi del singolo effetto né una garanzia per tutto il
gioco; non costituiscono un confronto controllato con la versione precedente.

La verifica della revisione espressiva è registrata separatamente in
[mgs-expressive-effects-evidence.json](mgs-expressive-effects-evidence.json),
con hash dei sorgenti, eseguibile, shader SPIR-V, test e acquisizioni UHD.
I tre tratti da savestate usano la cadenza NTSC normale; l'avvio completo
accelerato serve alla copertura e non al confronto delle prestazioni.
Le immagini e le istantanee diagnostiche restano locali sotto `data/native`.

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
