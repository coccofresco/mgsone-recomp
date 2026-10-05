# Effetti dello schermo nella presentazione Vulkan

La presentazione sostituisce gli attori riconosciuti con passaggi continui
sull'immagine Vulkan RGB8. La risoluzione predefinita rimane 3840×2160, 16:9;
il framebuffer software, i comandi GPU e la logica del gioco rimangono nativi.
Questi effetti sono attivi per impostazione predefinita nel profilo MGS USA.

## Luce per pixel e precisione del colore

Le mesh `SHADE` portano allo shader le normali originali KMD trasformate con
la posa float, anche alle giunzioni fra ossa. La luce viene calcolata nel
fragment shader dopo l’interpolazione prospettica e la rinormalizzazione:
non si interpolano più colori già illuminati ai vertici. La trasformazione
delle normali usa l’inversa trasposta, quindi conserva anche scale non uniformi.

Per gli oggetti dello scenario autenticati nella tabella `StageObjs` USA,
`DG_MakePreshade` non è la sorgente del colore moderno: si leggono i veri
`MAP.lit`, insieme a luce principale e ambiente. Posizione, direzione e
attenuazione sono valutate per pixel. Le luci locali sono caricate in SSBO,
deduplicate per disegno e selezionate con un AABB conservativo: non esiste
il limite originale di due luci. I materiali esplicitamente unlit e i colori
di attori senza questa attribuzione restano materiali, senza inventare luci.
Le matrici di luce personalizzate degli attori sono conservate; non tutte
sono automaticamente convertite in sorgenti puntiformi.

Il calcolo usa float e la presentazione usa attachment **RGB8, 24 bit colore**,
con modulazione che satura soltanto dopo luce × albedo, senza dithering PSX.
Non è un target HDR/FP16. Gli asset possono contenere palette originariamente
limitate: i bit bassi dei PCX MGS codificano anche STP e non sono una prova di
colore aggiuntivo. Palette, trasparenza e letture native restano intatte; la
presentazione non reinterpreta quei bit come dettaglio cromatico inventato.

La prova GPU `OptInGpuLightingIsPerPixelAndNativeRasterUnchanged` usa un quad
con quattro vertici ugualmente illuminati: il centro più luminoso dimostra
il calcolo per pixel. Verifica più di 32 gradazioni, identità dell’immagine
con dithering nativo acceso/spento, percorsi opaco/semitrasparente e tre luci
puntiformi RGB simultanee. La VRAM nativa resta identica. Le tabelle USA sono
controllate anche contro istruzioni precise dell’eseguibile originale.

Il checkpoint dell’intro è stato eseguito per 600 VBlank a UHD: 86.207 draw
con illuminazione per pixel, 99.771 draw a precisione RGB8, 39 passaggi zoom
blur e nessun downgrade del renderer. Evidenze e limiti sono in
[`mgs-modern-presentation-evidence.json`](mgs-modern-presentation-evidence.json).

## Percorsi implementati

| Origine | Resa Vulkan | Dati conservati dal gioco |
| --- | --- | --- |
| `wt_view.c` | Correnti bidimensionali a più scale, rifrazione con dispersione cromatica, caustiche animate e assorbimento azzurro | Attivazione, fase completa, area subacquea |
| `scn_mask.c`, visore notturno | Amplificazione del segnale, fosforo verde con alte luci gialle, diffusione luminosa a due scale, grana grossa animata e vignettatura; 113 primitive diventano un passaggio | Attore attivo, modalità e posizione nell'ordine di disegno |
| `scn_mask.c`, modo rosso durante una demo | Dominante rossa continua che conserva dettaglio e illuminazione della scena | Contesto `STATE_DEMO`, intensità del TILE e ordine di disegno |
| `scn_mask.c`, visore termico durante il gameplay | Palette dal blu freddo al magenta, giallo e bianco; emissione dei materiali IR | Modalità attiva fuori dalle demo e flag dei materiali originali |
| `fadeio.c` | Dissolvenza verso nero o bianco che conserva i rapporti fra i canali, senza saturarli uno alla volta | Intensità del TILE effettivamente disegnato, direzione, durata, pausa |
| `blurpure.c` | Scie direzionali con decadimento esponenziale, persistenza sensibile al movimento e lieve separazione cromatica delle code | Peso della scia, doppio buffer, ritaglio cinematografico 320×160 |
| `gas_efct.c` | Alone ellittico con gradiente continuo, senza i sedici settori poligonali | Raggio, proporzioni, colori animati interni/esterni, composizione additiva/sottrattiva |
| `blur.c` | Shutter radiale continuo con dieci campioni, senza giunzioni fra quadranti | Variante zoom, distorsione, crop e due framebuffer |
| `stgfd_io.c` | Sovraesposizione e diffusione ottica del lampo stun | Inviluppo RGB e durata originali; nessun timer di cecità aggiunto |
| `red_alrt.c` | Luce d’allarme pulsante con curva di esposizione continua | RGB, modo di composizione e impulso originali |
| `furnace.c` → `wt_view.c` | Turbolenza ascendente dell’aria calda, separata dalle caustiche subacquee | Proprietario del volume, map/name/bounds/color e fase |

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
`DG_FLAG_IRTEXTURE` ricevono un minimo emissivo quando `STATE_THERMG` è attivo
**fuori dalle demo**. Nelle cinematiche il medesimo codice IR è riutilizzato
per una dominante rossa: non rappresenta necessariamente la visione di un
sensore. La presentazione usa `CinematicRedTint` e conserva l’illuminazione
delle mesh senza corpi resi emissivi. Dopo il confronto visivo, le riprese
rosse e verdi delle DMO usano un trattamento CRT a bassa risoluzione con
scanline, disturbo e bloom, distinto dalla termografia di gameplay.

La revisione `9672b31` aveva interpretato erroneamente quella dominante
dell’intro come termografia e aggiunto un profilo caldo ai rig articolati.
Quell’euristica è rimossa. La RAM storica con `GM_GameStatus=0x80000028` e gli
attori `demothrd.c`, `goggleir.c`, `scn_mask.c` prova il riuso tecnico, non
l’intenzione termica della scena. La distinzione attuale usa il contesto DMO;
eventuali cinematiche che mostrino davvero un sensore richiederanno un
riconoscimento specifico, non l’inferenza dal solo flag IR.

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

La revisione espressiva storica aveva passato **277 test in 18 suite**. L'avvio completo di
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

La **correzione della dominante rossa del 4 ottobre** passa 77 test in quattro
suite, inclusi test GPU e riscontri sull’eseguibile/cache USA. Il checkpoint
dell’intro esegue 600 VBlank dopo il caricamento e produce quattro acquisizioni
3840×2160: 118 passaggi `cinematic_red_tint`, zero passaggi `thermal_vision`
e zero facce rese artificialmente emissive dal termico. Il fotogramma del
briefing a VBlank 9780 conserva la dominante rossa e i dettagli del corpo.
Nessun gruppo incompleto o ripiego del renderer; savestate personale e memory
card conservano gli hash precedenti. Le prove di separazione dal termico di
gameplay sono CPU/GPU, non una nuova partita con Therm.G equipaggiato.
Dettagli in [mgs-cinematic-red-evidence.json](mgs-cinematic-red-evidence.json).

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
Le precedenti catture dell’intro etichettate «termico» documentano il filtro
cinematico allora interpretato erroneamente: non certificano il visore termico
di gameplay. La copertura dei suoi materiali e cambi di palette va verificata
su scene di gioco con l’equipaggiamento attivo.

## Censimento dalla decompilazione e dalle fonti online — 4 ottobre 2026

Il [catalogo strutturato](mgs-effects-catalog.json) contiene **60 schede**:
famiglie di effetti, componenti condivisi e dossier sui boss. Non sono 60
effetti indipendenti: un boss riutilizza più famiglie e alcuni dossier
documentano una lacuna, non un renderer già identificato. Ogni scheda riporta
origine, meccanismo PSX, parametri da acquisire, proposta Vulkan, vincoli di
gameplay, priorità, stato attuale e situazione da riprodurre.

L’[indice delle fonti](mgs-effects-source-index.json) fissa hash, funzioni,
righe, permalink e registrazioni nelle tabelle degli stage. La scansione
esamina **741 file C**; 168 contengono i segnali grafici cercati, dei quali
126 sono collegati al catalogo e 42 hanno un’esclusione esplicita motivata
(infrastruttura, interfaccia ordinaria, entità, video, VR/debug). La lettura
dei percorsi indiretti porta a **200 file di riferimento** complessivi.
Questo chiude la classificazione di quella scansione, non prova che tutto
il codice grafico del gioco sia decompilato o coperto. In particolare,
`kogaku2`, `envmap3`, `sub_efct` e `bunsin2` mostrano perché cercare soltanto
allocazioni di primitive non basta.

La fotografia analizzata di
[FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing/tree/7b66372bcf156249560c90918d9472a47aabf3c5)
è `7b66372bcf156249560c90918d9472a47aabf3c5`. È stata estratta separatamente
in `.local/effects-research/mgs-upstream`: il checkout usato come donor
dal runtime resta a `217509eae471629b1a53633b816c61adfef9ed5c`.
È una fonte **Integral**: nomi e logica sono utili, indirizzi e strutture
USA vanno verificati. Le tabelle `stage/*.c` indicano che un costruttore è
disponibile, non che lo script lo esegua, né che una scena USA sia già provata.
`_stage.c`, essendo una tabella generale, non viene contato come scenario.

La ricerca online dà anche un riscontro concreto ai problemi delle copie
dello schermo: il manutentore di
[MGSM2Fix v3.0](https://github.com/nuggslet/MGSM2Fix/releases/tag/v3.0)
documenta correzioni dell’upscale per acqua, cinematiche e cifre Codec.
La [segnalazione #23](https://github.com/nuggslet/MGSM2Fix/issues/23)
riguarda zoom errato proprio durante cutscene e parti subacquee.
Questo sostiene la necessità di verificare origine, regione e ordine delle
letture del framebuffer moderno; non dimostra che quel codice sia un
adattatore Vulkan riutilizzabile direttamente.

Il manuale ufficiale descrive le funzioni di
[armi](https://metalgear.konami.net/manual/mc1/mgs1/pc/en/page16.html) ed
[equipaggiamento](https://metalgear.konami.net/manual/mc1/mgs1/pc/en/page17.html):
laser SOCOM, chaff, stun, guida Nikita e termico hanno significato di gameplay.
La reinterpretazione deve conservarlo. [Rex](https://github.com/Jayveer/Rex)
e [mint](https://github.com/virusek20/mint) aiutano a reperire/analizzare gli
asset; non certificano la semantica degli effetti. Non sono state importate
attribuzioni di pioggia, shader o materiali da MGS2/MGS3.

### Risultati della ricognizione iniziale (storico)

I punti seguenti descrivono il censimento precedente agli adattatori. Lo stato
implementato è riportato nella sezione finale e nel catalogo JSON aggiornato.

1. **Acqua e fonderia condividono un attore.** `furnace.c` chiama
   `NewWaterView2` con il suo volume e un colore iniziale 64/10/10.
   `captureWaterEffects` riconosce `wt_view.c` e trasferisce la fase, ma
   non identifica il chiamante Furnace. È quindi un rischio concreto di
   classificazione da verificare in `s13a` USA, non una regressione visiva
   già riprodotta in questa ricerca. Serve distinguere il trattamento
   termico da caustiche e assorbimento subacquei. Il richiamo avviene nel
   volume definito dall’attore: non prova una foschia globale della stanza.
2. **La mimetica ottica è un materiale dinamico.** `kogaku2.c` modifica UV e
   tpage del corpo per campionare il framebuffer opposto, con tinte e soglie
   di rivelazione. Va ricostruita dalla mesh e dai parametri vivi. Un filtro
   a pieno schermo o una texture KMD congelata non descrivono l’effetto.
3. **Esistono due tipi diversi di scia.** `blurpure.c` accumula immagini;
   `animal/ninja/bunsin2.c` conserva 32 campioni di world matrix e pose di
   16 ossa, per disegnare un duplicato ritardato. Quest’ultimo richiede
   storia delle pose, geometria e occlusione proprie, utili anche al futuro
   campionamento 2×.
4. **La messa a fuoco usa la ordering table.** `focus.c` seleziona intervalli
   near/far tramite STP e ricopie traslate. Una DOF moderna deve derivare
   una distanza/coerenza di fuoco da quei parametri: non basta sfocare tutto
   il frame. `blur.c`, separatamente, deforma quattro quadranti e ha più modi.
5. **La nebbia modifica la pipeline.** `fog.c` sostituisce bound, transform,
   shade e sort, ed è sensibile allo zoom. Il fix precedente che acquisisce
   tutte le mesh in `s12c` risolveva la cattura, non introduceva scattering o
   nebbia moderna. Il costruttore è registrato anche in altri stage, inclusa
   la fuga `s19b/s19br`.
6. **Gli effetti di superficie restano famiglie distinte.** `wsurface`
   anima UV su una griglia; `ripple/rsurface` genera onde da contatti;
   `sub_efct` cambia UV e luce sul sottomarino; `mirror` duplica oggetti
   rispetto a un piano; `envmap3` genera UV dalle normali. Possono condividere
   attachment e materiali, ma non un solo filtro indistinto.
7. **I nomi possono trarre in inganno.** `gsplash` usa `glass` per i frammenti
   di vetro; `splash` usa `blood_1`, mentre `splash2/3` usano `awa_3` e
   `splash2` può generare onde. `thing/emitter` disegna bagliori senza una
   funzione Act; il motore comune delle animazioni è `anime/animconv`.
   `dymc_flr/dymc_seg` riguardano collisioni HZD, non un nuovo shader.
8. **Alcune lacune sono regionali o di decompilazione.** `mosaic.c` è in
   gran parte stub nella fonte Integral; il collegamento a Johnny è presentato
   dagli autori come ipotesi. Le stringhe `hideo_b/hideo_s/hideo` sono in
   `psyco.c`, ma molte funzioni sono ASM. `DEMO_METALGEARLASER` è ancora
   irrisolto; il README segnala assenza di decompilazione per gli overlay
   REX, Liquid e garage. Il registro conserva 143 costruttori `?`, inclusi
   attori non grafici e varianti PC/VR: **non sono 143 effetti mancanti**.

### Interventi creativi sensati per famiglia

| Ambito | Trattamento Vulkan proposto | Dati originali indispensabili |
| --- | --- | --- |
| Mimetica e energia del Ninja | Rifrazione sulla mesh, bordi energetici, scie da pose storiche, archi luminosi continui | Corpo, ossa, fase, soglie di rivelazione, centri e durata degli attacchi |
| Acqua, metallo fuso, specchi | Superfici con normali continue, Fresnel, rifrazione, riflesso planare; profilo termico distinto | Piano/bounds, camera, materiale, oggetti riflessi e contatti |
| Ottiche cinematiche | DOF da profondità, zoom blur senza quadranti, scie con disocclusione | Bersaglio/range del fuoco, modo blur, history, ritagli e camera cut |
| Atmosfera | Nebbia per pixel, fiocchi in spazio mondo, fumo/vapore illuminati e intersezioni morbide | Distanza/zoom, volumi, vento, tipo di emissione, età e densità |
| Esplosioni e armi | Flash coordinati, fumo, scintille, traccianti e luci transitorie | Evento del colpo, volata, traiettoria e impatto nativi |
| Lampade e fasci | Luci per pixel, coni leggibili, riflessi e bloom selettivo | Matrice, colore e volume originali; condizioni di visibilità dei sensori |
| Impronte, sangue e detriti | Decal aderenti, gocce e frammenti senza coordinate intere a schermo | Superficie, contatto, durata e variante; le impronte conservano valore di indizio |
| Texture animate e monitor | UV continui, palette vive, display su render target dedicati | Identità asset, upload/CLUT, fase, contenuto e stato rotto/intatto |
| Visori e interferenze | Materiali IR verificati, grana leggibile, disturbi distinti per sensore/radar | Modalità, identità oggetti, palette, stato chaff e informazioni oscurate |
| Mirini, segnali, titoli | Linee/forme nitide alla risoluzione finale, animazione originale | Zoom, lock-on, testo/asset e tempi; nessuna deformazione da resize |

Queste sono proposte di resa, non funzionalità abilitate da questo documento.
Il replay generico di un quad o di una particella in Vulkan non equivale a
un adattatore semantico moderno. I video MDEC e il dettaglio mancante nelle
bitmap non diventano ricostruibili dalla sola decompilazione.

### Percorso unitario e verifica senza partita continua

L’unità da consegnare alla presentazione dovrebbe essere una descrizione
dell’effetto: identità dell’attore/animazione, owner, trasformazione, parametri,
fase, materiale e dipendenze di composizione. I parametri vanno copiati prima
della riduzione in packet PSX. L’identità deve includere overlay/versione e
vita dell’attore: un indirizzo RAM può essere riutilizzato dopo un caricamento.

Tre percorsi condivisi coprono la maggior parte del lavoro: effetti di
schermo ordinati sugli attachment moderni; materiali/superfici collegati alla
geometria originale; particelle, decal e curve alimentate da ANIMATION,
PRESCRIPT e attori specializzati. Le varianti jeep e boss devono convergere
sugli stessi renderer attraverso adattatori verificati dei rispettivi dati.
Non è opportuno forzare scie di mesh, luci o impronte in un postprocess.

La dominante rossa dell’intro è stata separata dal termico a seguito della
correzione dell’utente. È lo stesso problema di attribuzione semantica che
deve guidare gli altri adattatori: identificare codice e flag non basta a
identificare il significato visivo nella scena.

La sequenza seguita durante l’implementazione è stata: discriminare Furnace/WaterView; introdurre
l’identità semantica delle animazioni; portare Kogaku/Bunsin; completare
Focus/Blur/Fog e flash stun. Seguono superfici/specchi, particelle/luci e
varianti boss. Le lacune REX/HIDEO si investigano con trace e script USA,
senza attendere o ripercorrere una partita intera.

| Situazione di prova | Base disponibile o indizio | Cosa manca per chiamarla copertura |
| --- | --- | --- |
| Intro: acqua, bolle, sottomarino, display, visori, scie | Acquisizioni e prove storiche della presentazione | Nuove acquisizioni dopo ogni adattatore; le prove storiche non certificano gli effetti nuovi |
| Fonderia e nebbia Disc2 | Seed `d2-01-s13a-204`, `d2-02-s14e-204` | Entrare nei volumi/condizioni corrette e registrare attori/packet realmente presenti |
| Neve e black fog | Seed `disc-change`, `s11c/s12b`; tabella `s12c` | Campo largo/zoom, nemico distante e interazione con trasparenti |
| Specchi | Registro Integral `s07a/s07c/s07cr`; seed `d1-06-s07c-119` | Confermare attivazione USA e oggetti riflessi, anche con stealth |
| Superfici d’acqua e sensori | Costruttori registrati in `s02c/s02d/s02e`; sensori anche `s04a` | Preparare snapshot USA con quei costruttori attivi |
| Ninja e Mantis | Fonti `s08b` e `s07b` | Snapshot delle fasi dei boss; il corridoio `s08c` o una stanza dopo lo scontro non bastano |
| REX, Liquid, garage e jeep | Seed Disc2 e sorgenti/registri parziali | Provare separatamente attacchi, fasi e transizioni; avviare la mappa non certifica l’effetto |
| Visori, stun, chaff, mirini, Codec | Harness `scripts/mgs_situations.py` e seed già disponibili | Equipaggiamento/condizioni applicabili, seguiti da confronto grafico dedicato |

Per ogni effetto: attivazione, regime, fine, pausa, reload e almeno una
composizione problematica (trasparente, HUD, camera cut o altro effetto).
La situazione deve prima provare la presenza dell’attore e dei parametri;
se manca il trigger, il risultato è **non applicabile**, non un successo.
Confrontare donor e nativo per logica/eventi, e presentazione originale/moderna
per resa e leggibilità. Solo gruppi di packet interamente attribuiti possono
essere soppressi; contare anche gruppi non riconosciuti, incompleti e duplicati.
La durata, il RNG e le letture/scritture native restano invariati.

L’accesso ai formati originali si distribuisce così; il contratto degli effetti
deve conservarne l’identità senza appiattire tutto in packet già proiettati:

| Fonte originale | Uso nella presentazione |
| --- | --- |
| KMD, normali e pose OAR/stato delle ossa | Corpo rifrangente, riflessi, ombre, punti di emissione e duplicati delle pose |
| DMO e comandi GCL | Attivazione, camera e composizione delle cinematiche; l’esecuzione rimane alla logica del gioco |
| ANIMATION/PRESCRIPT e relativo bytecode | Famiglia, atlante, posizione/velocità, fase e vita delle particelle; non è necessario inventare un nuovo file di animazione originale |
| Texture originali e CLUT vive | Materiali, emissione, palette dei visori, animazioni e display; gli aggiornamenti runtime restano osservabili |
| DG_IMG e dati dei mirini DG_SGT | Panorami e forme dell’interfaccia senza quantizzazione delle coordinate alla risoluzione PSX |
| HZD e controlli/target | Superficie d’appoggio, bounds e condizioni originali; non sostituiscono la geometria visiva KMD |

Le verifiche degli scenari già registrate in
[mgs-situation-evidence.json](mgs-situation-evidence.json) sono una base per
preparare questi casi: non attestano che i 60 dossier siano già verificati
visivamente. Il censimento iniziale era solo documentale; la correzione
successiva della dominante rossa modifica il runtime e ha prove separate.

### Riproduzione dell’audit delle fonti

Il catalogo è la parte curata; lo script rigenera l’indice con hash, segnali
grafici, punti d’ingresso e registrazioni. `--check` verifica il contenuto già
registrato senza scrivere. Fallisce su fonti diverse dalla fotografia fissata,
riferimenti/ancore mancanti, ID duplicati o nuovi candidati non classificati.
Non è un parser completo C/GCL e non sostituisce l’analisi del codice ASM.

```bash
# Recuperare la fotografia senza spostare il checkout del donor.
git -C .local/mgs_reversing fetch origin master
mkdir -p .local/effects-research/mgs-upstream
git -C .local/mgs_reversing archive \
  7b66372bcf156249560c90918d9472a47aabf3c5 source build README.md \
  | tar -x -C .local/effects-research/mgs-upstream

python3 scripts/mgs_effects_catalog.py --check
# Dopo una modifica intenzionale e revisionata al catalogo:
python3 scripts/mgs_effects_catalog.py
```

### Indice delle 60 schede

Il riferimento è un punto d’ingresso; file complementari, varianti, parametri e
prove sono nel catalogo. Gli stati «da adattare» non significano che l’elemento
sia invisibile: può già essere disegnato attraverso il replay generico.

| Scheda | Fonte di partenza | Stato |
| --- | --- | --- |
| `water_view` — Ottica subacquea | [takabe/wt_view.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/wt_view.c#L250) | P1 · Adattatore implementato |
| `furnace_heat` — Calore della fonderia e immersione nel metallo | [takabe/furnace.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/furnace.c#L148) | P0 · Adattatore implementato |
| `night_vision` — Visore notturno | [takabe/scn_mask.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/scn_mask.c#L142) | P1 · Adattatore implementato |
| `cinematic_red_tint` — Dominante rossa cinematografica | [takabe/goggleir.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/goggleir.c#L143) | P0 · Adattatore implementato |
| `thermal_vision` — Visore termico e materiali IR | [equip/gglmng.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/equip/gglmng.c#L22) | P1 · Adattatore implementato |
| `fade` — Dissolvenze cinematiche | [takabe/fadeio.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/fadeio.c#L43) | P1 · Adattatore implementato |
| `temporal_trails` — Scie e persistenza temporale | [okajima/blurpure.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/blurpure.c#L127) | P1 · Adattatore implementato |
| `zoom_blur` — Zoom, sfasamento e deformazione a quadranti | [okajima/blur.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/blur.c#L242) | P0 · Adattatore implementato |
| `focus` — Messa a fuoco selettiva | [takabe/focus.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/focus.c#L29) | P0 · Adattatore implementato |
| `stun_flash` — Flashbang e abbagliamento | [okajima/stgfd_io.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/stgfd_io.c#L32) | P0 · Adattatore implementato |
| `red_alert` — Lampeggi e lavaggi cromatici d’allarme | [okajima/red_alrt.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/red_alrt.c#L104) | P1 · Adattatore implementato |
| `sepia` — Monocromia e viraggi | [takabe/palette.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/palette.c) | P1 · Adattatore implementato |
| `gas_halo` — Aloni e velature di gas | [takabe/gas_efct.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/gas_efct.c#L94) | P1 · Adattatore implementato |
| `mosaic` — Mosaico e censura regionale | [takabe/mosaic.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/mosaic.c#L37) | P2 · Adattatore implementato |
| `optical_camo` — Mimetica ottica e rivelazione del corpo | [equip/kogaku2.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/equip/kogaku2.c#L116) | P0 · Adattatore implementato |
| `env_mapping` — Riflessi dipendenti dalle normali | [takabe/envmap3.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/envmap3.c#L230) | P1 · Adattatore implementato |
| `planar_mirror` — Specchi e oggetti riflessi | [takabe/mirror.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/mirror.c#L125) | P1 · Adattatore implementato |
| `water_surface` — Superficie dell’acqua | [takabe/wsurface.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/wsurface.c#L124) | P1 · Adattatore implementato |
| `ripples` — Onde concentriche da contatto | [takabe/ripple.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/ripple.c#L92) | P1 · Adattatore implementato |
| `submarine_material` — Scorrimento luminoso sul sottomarino | [takabe/sub_efct.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/sub_efct.c#L71) | P1 · Adattatore implementato |
| `distance_fog` — Nebbia di profondità e black fog | [takabe/fog.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/fog.c#L53) | P0 · Adattatore implementato |
| `snow` — Neve e tormenta | [kojo/sstorm.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/kojo/sstorm.c#L90) | P1 · Adattatore implementato |
| `bubbles` — Bolle e scie subacquee | [okajima/bub_d_sn.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/bub_d_sn.c#L49) | P1 · Adattatore implementato |
| `smoke_steam` — Fumo, vapore, polvere e scie persistenti | [anime/effect/fog.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/effect/fog.c#L31) | P1 · Adattatore implementato |
| `breath_cigarettes` — Condensa del respiro e sigarette | [anime/effect/breath.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/effect/breath.c#L57) | P1 · Adattatore implementato |
| `explosions_fire` — Esplosioni, fiamme e onde d’urto | [anime/effect/bullet.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/effect/bullet.c#L309) | P1 · Adattatore implementato |
| `muzzle_flashes` — Vampe delle armi e illuminazione istantanea | [anime/effect/bullet.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/effect/bullet.c#L309) | P1 · Adattatore implementato |
| `projectiles` — Traccianti, missili e traiettorie visibili | [bullet/amissile.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/bullet/amissile.c#L213) | P1 · Adattatore implementato |
| `sparks` — Scintille, urti e rimbalzi | [okajima/spark.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/spark.c#L216) | P1 · Adattatore implementato |
| `electricity` — Archi elettrici, scariche e plasma | [okajima/death_sp.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/death_sp.c#L39) | P1 · Adattatore implementato |
| `blood_spray` — Sangue in aria e gocce | [kojo/blooddrp.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/kojo/blooddrp.c#L50) | P1 · Adattatore implementato |
| `liquid_decals` — Pozze, macchie e cerchi di liquido | [okajima/blood_bl.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/blood_bl.c#L54) | P1 · Adattatore implementato |
| `glass_shards` — Vetri e frammenti | [takabe/gsplash.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/gsplash.c#L234) | P1 · Adattatore implementato |
| `debris` — Detriti, spruzzi e materiali espulsi | [okajima/splash.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/okajima/splash.c#L112) | P1 · Adattatore implementato |
| `footprints` — Impronte di gameplay e cinematiche | [enemy/asiato.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/enemy/asiato.c#L55) | P1 · Adattatore implementato |
| `shadows` — Ombre dei personaggi | [chara/snake/shadow.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/chara/snake/shadow.c#L169) | P1 · Adattatore implementato |
| `lights_glows` — Luci locali, bagliori e lampeggianti | [thing/emitter.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/thing/emitter.c#L109) | P1 · Adattatore implementato |
| `searchlights` — Proiettori di ricerca e coni luminosi | [enemy/searchli.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/enemy/searchli.c#L760) | P1 · Adattatore implementato |
| `lasers` — Laser, sensori infrarossi e puntamento | [enemy/camera.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/enemy/camera.c#L752) | P1 · Adattatore implementato |
| `texture_animation` — Texture scorrevoli e palette animate | [equip/effect.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/equip/effect.c) | P0 · Adattatore implementato |
| `monitors` — Monitor, display del sottomarino e della base | [game/evpanel.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/game/evpanel.c#L393) | P1 · Adattatore implementato |
| `sky_panorama` — Cielo e panorami avvolgenti | [thing/sphere.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/thing/sphere.c#L51) | P1 · Adattatore implementato |
| `wind_rings` — Anelli di vento e spostamento d’aria | [takabe/windcrcl.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/windcrcl.c#L124) | P1 · Adattatore implementato |
| `ninja_special` — Ninja: lampi, spada e sfera energetica | [animal/ninja/tenmetu.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/animal/ninja/tenmetu.c#L152) | P0 · Adattatore implementato |
| `mantis_special` — Mantis: HIDEO, telecinesi e onde | [overlays/s07b/chara/psyco/psyco.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/overlays/s07b/chara/psyco/psyco.c#L158) | P0 · Adattatore implementato |
| `tank_hind_special` — Tank e Hind: composizione degli effetti | [chara/hind2/hind2.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/chara/hind2/hind2.c#L532) | P1 · Adattatore implementato |
| `raven_special` — Vulcan Raven e magazzino | [overlays/s15c/okajima/valcan/valcan.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/overlays/s15c/okajima/valcan/valcan.c#L244) | P1 · Adattatore implementato |
| `rex_unknown` — REX: laser, armi e scarichi USA | [include/charalst.h](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/include/charalst.h#L340) | P0 · Adattatore implementato |
| `liquid_garage_unknown` — Liquid e garage: mesh ed emissioni condivise | [include/charalst.h](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/include/charalst.h#L36) | P0 · Adattatore implementato |
| `jeep_variants` — Fuga in jeep: fari, fuoco, vetro e colpi | [overlays/s19b/anime/effect/janime2.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/overlays/s19b/anime/effect/janime2.c) | P1 · Adattatore implementato |
| `head_marks` — Segni di allerta, sonno e indicatori narrativi | [anime/effect/mark.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/effect/mark.c#L157) | P1 · Adattatore implementato |
| `codec_radar` — Codec, interferenze radar e composizione dell’interfaccia | [menu/radar.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/menu/radar.c) | P0 · Adattatore implementato |
| `optical_sights` — Mirini e ottiche di equipaggiamento | [equip/gglsight.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/equip/gglsight.c#L391) | P1 · Adattatore implementato |
| `camera_shake` — Scuotimenti e tagli della camera | [takabe/camshake.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/camshake.c#L31) | P1 · Adattatore implementato |
| `cinema_bands` — Bande cinematografiche e aperture | [takabe/cinema.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/takabe/cinema.c#L46) | P2 · Adattatore implementato |
| `titles_gameover` — Titoli, game over, telop e cambio disco | [game/over.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/game/over.c#L434) | P2 · Adattatore implementato |
| `photo_camera` — Fotocamera e acquisizioni della scena | [equip/jpegcam.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/equip/jpegcam.c#L924) | P2 · Adattatore implementato |
| `procedural_geometry` — Mantelli e deformazioni delle mesh | [onoda/s04b/cape.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/onoda/s04b/cape.c#L489) | P2 · Adattatore implementato |
| `animation_vm` — Motore comune degli effetti animati | [anime/animconv/anime.h](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/anime/animconv/anime.h#L18) | P0 · Adattatore implementato |
| `pose_afterimage` — Duplicati temporali delle pose del Ninja | [animal/ninja/bunsin2.c](https://github.com/FoxdieTeam/mgs_reversing/blob/7b66372bcf156249560c90918d9472a47aabf3c5/source/animal/ninja/bunsin2.c#L54) | P0 · Adattatore implementato |

## Criterio di completamento della presentazione

L’equivalenza richiesta è **visiva e funzionale**, non fedeltà alla tecnica
PSX: significato, attivazione, durata e leggibilità devono rimanere corretti,
mentre luce, ottica, rifrazione e composizione possono essere reinterpretate.
Questo vincolo è ora obbligatorio nelle 60 schede del catalogo.

`python3 scripts/mgs_effects_catalog.py --check` verifica inventario,
contratti e riferimenti al renderer. Aggiungendo `--require-complete` il
controllo fallisce finché tutte le schede non hanno renderer e prove delle
scene pertinenti. Un replay generico o uno shader soltanto compilato non
costituiscono copertura completa. Gli adattatori delle 60 schede sono ora implementati, mentre la verifica
visiva di tutte le scene e combinazioni rimane incompleta. Per controllare
solo la consegna degli adattatori usare `--require-adapters`; il controllo
`--require-complete` mantiene anche il requisito delle scene e deve ancora
fallire. I dossier si sovrappongono: non si tratta di 60 shader indipendenti.


## Consegna degli adattatori — 4 ottobre 2026

I dettagli verificabili sono in [mgs-effects-adapter-evidence.json](mgs-effects-adapter-evidence.json).
Il catalogo descrive il comportamento implementato, separato dalle proposte
creative della ricognizione storica riportata sopra.

La cattura legge attori, DG_PRIM/DG_MDL, animazioni, trasformazioni e parametri
originali. Copia i dati destinati alla GPU e valida liste, allocazioni, opcode,
texture e provenienza. Gli indirizzi USA degli overlay non vengono dedotti
dai suffissi della decompilazione Integral. Le 77 firme delle animazioni
richiedono l’intero bytecode, atlante, ID texture e cursore; SnowStorm ammette
come variabile solo l’operando di durata costruito dal gioco.

Gli effetti condividono tre percorsi: ottiche di schermo sulla scena/depth
Vulkan; materiali sulle mesh originali; particelle, decal e linee dai dati
originali. Le varianti dei boss e della jeep alimentano questi percorsi. Il
laser generato di REX e i volumi di ricerca decodificano direttamente i modelli
indicizzati in RAM. Il mantello ricostruisce normali condivise dai vertici
animati. Gli specchi mantengono le trasformazioni riflesse originali e
aggiungono il clipping del piano, anche insieme alla mimetica.

La composizione usa colore RGB8 e calcoli float senza dithering PSX. Le luci
SHADE e .LIT vengono valutate per pixel. Le texture conservano il contenuto
originale: questo non ricrea dettaglio assente nelle bitmap, nei font o nei
video MDEC. Ombre di contatto, bagliori, acqua, vetro, mimetica, fumo, neve,
archi e segnali hanno profili distinti; il rosso cinematografico resta distinto
dal termico di gameplay.

Le linee dell’interfaccia sono nastri continui a risoluzione finale. Gli SGT
riconosciuti e gli array dei mirini conservano le proporzioni in 16:9, con le
maschere esterne estese alla finestra. Il panorama conserva le immagini e la
fase originale, recuperando offset frazionari e bordi delle tile. La GPU
accetta questi dati 2D senza attribuire loro una falsa proiezione GTE.

La macchina fotografica può esportare uno scatto RGB8 della presentazione in
`data/presentation/photos/` (PPM, directory configurabile con
`PS1_MGS_PHOTO_DIRECTORY`). Lo scatto nativo, i fantasmi e l’album su memory
card continuano a usare la logica del gioco. Il caricamento di uno stato già
alla fase dello scatto non genera una nuova fotografia.

La verifica combina test di provenienza e rifiuto delle identità errate,
prove GPU di luce/colore/composizione con confronto della VRAM nativa e brevi
esecuzioni reali da checkpoint. La stanza contenente un attore non prova che
il suo effetto sia apparso: le acquisizioni riportano i contatori effettivi.
La visita al corridoio di `s07c`, per esempio, non attiva i riflessi dello
specchio; la composizione specchio/mimetica ha una prova sintetica dedicata.

Shadow map aggiuntive, motion vectors e interpolazione 2x restano sviluppi
successivi. I tempi, il RNG, i danni, l’AI e le letture/scritture PSX non sono
sostituiti dagli adattatori della presentazione.


## Revisione visiva di acqua, bolle e riprese CRT — 4 ottobre 2026

La correzione segue il confronto con i fotogrammi nativi dell’intro:

- Acqua: ampiezza della rifrazione ridotta a un decimo (massimo teorico
  inferiore a 0,7 pixel nativi), assorbimento e ombre più profonde. Nessuna
  caustica sullo schermo né velatura additiva che sollevi i neri.
- Sottomarino: rimosse le caustiche in UV, che si stiravano lungo lo scafo.
- Bolle al VBlank 9720: la sfumatura contro la profondità ignorava
  `DG_PRIM.raise`. `bubble_p` vale 3000 nella RAM USA e nella decompilazione;
  `bub_d_sn` vale 300. Il solo confronto ottico usa tale offset; posizioni,
  quantità, tempi e ordinamento nativi rimangono gli input originali.
- Riprese verdi/rosse delle DMO: segnale video 320×112 dalla scena moderna,
  ricostruito sulla superficie 4K con scanline, grana e bloom diffuso.
  Il tempo viene da `GV_Time`, con pausa/rewind riproducibili. Il visore
  notturno e il termico di gameplay conservano i loro profili separati.
- Neve: nessuna modifica; la relativa cattura resta quella già approvata.

Le prove aggiungono un caso GPU con bolle dietro una superficie opaca:
sono occultate con offset zero e visibili con l’offset originale, mantenendo
la VRAM PSX identica. La prova CRT verifica perdita deliberata del dettaglio
subpixel, energia luminosa attorno alla sorgente, scanline, pausa/rewind e
separazione dal termico. La suite completa conta 354 test in 28 suite.

La [galleria revisionata](../data/presentation/effects-comparison-20261004-v2/index.html)
contiene undici coppie originali/Vulkan, abbinate entro la stessa esecuzione
allo stesso VBlank. I dati riproducibili e gli hash sono in
[mgs-effects-refinement-evidence.json](mgs-effects-refinement-evidence.json).
Le evidenze precedenti restano storiche; non certificano gli shader revisionati.

## CRT, caustiche e bolle ottiche — revisione 3, 4 ottobre 2026

Il CRT delle DMO usa un fascio gaussiano la cui larghezza cresce con la luce,
una maschera fine del fosforo e una tenue instabilità di riga legata al tempo
del gioco. Il bloom distingue la diffusione vicina (3,2 pixel nativi) dall’alone
nel vetro (13 pixel); le alte luci hanno una risposta cromatica più luminosa.
La risoluzione video intenzionale resta 320×112, composta nella superficie 4K.

Le caustiche sullo scafo tornano come celle grandi nello spazio della scena,
proiettate sui tre piani e pesate dalle normali. Non dipendono dalle UV
allungate delle texture. La luce è concentrata sui bordi delle celle; il
contrasto scuro del fondale e la rifrazione tenue della revisione 2 restano.

La lettura dei PCX originali ha chiarito un errore dell’adattatore: `awa_1`,
`awa_2` e `awa_3` contengono gruppi di bollicine, non singole sfere. Il comando
`python3 scripts/mgs_bubble_layouts.py --check` verifica le disposizioni
ricavate dalle componenti luminose (6, 12 e 25 forme). I soli centri/raggi sono
nel GLSL; immagini e palette rimangono negli asset locali. `bubble_p`,
`bub_d_sn` e `splash2` selezionano la disposizione dai rispettivi layout
validati; le varianti `awa_s` conservano la singola bolla.

Ogni bolla ha un guscio trasparente, rifrazione della scena, risposta Fresnel,
riflessi direzionali e piccoli punti luce bianchi. Il replay usa composizione
alpha premoltiplicata sul colore corrente: non ricopia la scena immutabile
sopra alle particelle già disegnate. Il percorso con feedback esplicito calcola
la stessa miscela. L’offset `DG_PRIM.raise` resta nel confronto di profondità;
la GPU nativa mantiene i propri comandi e la propria VRAM.

Le bolle consecutive, anche di attori diversi, condividono la scena ottica
prima della nube. Un altro materiale, una scrittura ordinaria o un cambio di
buffer forza una nuova copia. Nei due tratti diagnostici da 600 VBlank, il
numero di copie e il tempo GPU medio si riducono: nel tratto subacqueo da
9.944 a 678 copie e da 40,19 a 23,90 ms; nel tratto rosso/verde da 24,89 a
12,16 ms. Il confronto include anche la ricostruzione dei gruppi e il calcolo
selettivo delle proiezioni delle caustiche: non isola il costo di un singolo
effetto né costituisce una misura degli FPS dell’intero gioco.

356 test in 28 suite sono passati. La prova GPU richiede riflessi delle bolle
oltre 160/255, centro trasparente, angoli dello sprite invariati e VRAM nativa
identica. Le nuove prove verificano disposizione dei gruppi e invalidazione
della scena ottica. Entrambe le run hanno prodotto cinque coppie PSX/Vulkan
senza ripieghi del renderer. Neve e salvataggi personali sono invariati.

[Galleria della revisione 3](../data/presentation/effects-comparison-20261004-v3/index.html)
e [evidenze e hash](mgs-effects-optics-v3-evidence.json).

## Volume delle bolle, profondità e leggibilità — revisione 4, 4 ottobre 2026

Le bolle conservano disposizioni e traiettorie originali, ma il centro ora ha
un corpo blu-bianco luminoso e una copertura sostanziale. Il guscio mantiene
rifrazione, riflessi e punti luce: le bolle piccole restano leggibili anche
quando l’immagine 4K viene ridotta sul monitor. Le caustiche passano da celle
di circa 714 a 455 unità della scena, con intensità quasi dimezzata e alone
meno marcato; la proiezione resta indipendente dalle UV.

L’acqua legge la profondità R32 della scena e applica estinzione esponenziale
oltre 2500 unità: il rosso si attenua prima del verde e del blu, verso un
mezzo blu molto scuro. Gli elementi privi di profondità restano esclusi.
Il passaggio `blur.c` prima invalidava quella profondità; ora
conserva la profondità della scena corrente per eventuali effetti successivi,
pur mantenendo invariata la politica del buffer operativo di occlusione.
Le scie storiche non hanno una propria geometria di profondità: si usa
quella della scena corrente. I pixel trasparenti con profondità non risolta
rimangono esclusi dalla foschia anziché ricevere una distanza inventata.
Il calore della fornace condivide l’attore ma non usa la foschia subacquea.

Le due riprese CRT hanno tarature separate. Il verde usa soglie inferiori,
più energia nel bloom e un alone largo 15 pixel nativi. Il rosso riduce
il bloom, solleva i mezzi toni, attenua l’oscuramento delle scanline e usa
una risposta esponenziale per evitare alte luci rosse piatte. Nessun cambio
ai visori di gameplay, alla neve, al tempo del gioco o alla VRAM nativa.

357 test in 28 suite passati. La nuova prova GPU confronta superfici uguali
vicine/lontane, una superficie senza profondità e il profilo fornace, poi
ripete il confronto vicino/lontano dopo un vero passaggio zoom. Verifica
anche VRAM nativa identica e assenza di ripieghi del renderer. La prova delle
bolle richiede ora un centro pieno oltre 100/255, riflessi più luminosi e
angoli trasparenti.

[Galleria della revisione 4](../data/presentation/effects-comparison-20261004-v4/index.html)
e [evidenze e hash](mgs-effects-optics-v4-evidence.json). Il confronto copre
le riprese acquisite dell’intro; la neve riusa la cattura già approvata.

## Ritorno ai materiali e alla risposta cromatica originali — revisione 5

Il confronto dell’utente ha evidenziato che le sfere blu opache e il reticolo
cellulare delle revisioni precedenti cambiavano troppo il carattere visivo.
Le bolle ora ricostruiscono direttamente i PCX originali con filtro continuo
in RGB8: conservano contorni irregolari, bolle grandi cave, particelle piccole
piene e palette. `bubble_p` e `bub_d_sn` tornano alla somma di un quarto della
luce; `bubble_s` e gli splash conservano la propria somma intera. Le nubi
si accumulano senza coprire la scena con sfere opache. Disposizioni, traiettorie,
sfumatura di intersezione e offset `DG_PRIM.raise` restano gli input originali.
Le forme analitiche restano solo come ripiego per packet privi del PCX associato.

Rimosso il reticolo procedurale sul sottomarino. La decompilazione
`SubEfct_800CC798` descrive la proiezione `u = 128 + RotZ(rotation).x / 8`:
il runtime legge la rotazione dell’attore e la applica alla geometria sorgente
in virgola mobile; Vulkan ricostruisce la trama originale per frammento.
Il wrapping avviene al campionamento, senza interpolare UV già troncate a byte.
Il materiale originale conserva il proprio piano STP e la composizione
traslucida sopra allo scafo. Non si inventano nuovi filamenti luminosi.

`ScnMask` aggiunge (80,0,0) nel rosso e (56,120,40) nel verde, anche sui neri.
La conversione monocromatica delle revisioni precedenti perdeva questa base
cromatica: il bloom non poteva recuperare le sagome lasciate nere. Ora la
presentazione conserva RGB della scena e base cromatica originale, con fascio
CRT, rumore legato al tempo del gioco e bloom a punto di bianco più basso
(0,32 rosso / 0,22 verde). La luce diffusa viene dalle sorgenti della scena;
la base cromatica non viene sommata ricorsivamente al bloom.

Le prove includono rotazione e traslazione della proiezione dello scafo,
conservazione dei valori frazionari e rifiuto di puntatori invalidi; il test
GPU verifica anche la luminanza media delle ombre colorate, oltre a rumore,
pausa, rewind e VRAM nativa identica. Il disegno dei PCX mantiene la propria
risoluzione sorgente: il filtro continuo non inventa dettaglio assente.
Passati 358 test in 28 suite; due acquisizioni da 600 VBlank hanno prodotto
dieci nuove coppie PSX/Vulkan 4K senza ripieghi del renderer.

[Galleria della revisione 5](../data/presentation/effects-comparison-20261004-v5/index.html)
e [evidenze e hash](mgs-effects-optics-v5-evidence.json). Acqua, fog e neve
conservano la taratura precedente. Le evidenze delle revisioni 3 e 4 restano
storiche e non descrivono la nuova composizione delle bolle.

## Taratura intermedia — revisione 6

Su richiesta dell’utente, la resa combina la definizione della revisione 4
con il carattere originale recuperato nella revisione 5. Le bolle mescolano
in parti uguali disegno originale e illuminazione continua di corpo, bordo e
riflesso, sempre limitata dalla sagoma originale. Si mescolano le
due risposte di composizione: metà luce additiva/quarto-additiva originale
e metà corpo illuminato, con un assorbimento massimo del 50% per strato
che segue sagoma, dissolvenza e intersezioni. Il contributo moderno
ha riflessi meno saturi: le bolle sovrapposte recuperano volume e definizione.
I percorsi Vulkan rapido e preciso applicano l’assorbimento al colore vivo
della destinazione; VRAM e simulazione native rimangono originali.

La trama luminosa originale sullo scafo ha guadagno 1,65 e un piccolo aumento
delle alte luci ricavato dalla sua luminanza. Il CRT riduce la base cromatica
all’82% nel rosso e al 60% nel verde, con fascio un po’ più stretto e punto
di bianco 0,37/0,27. Il verde recupera contrasto; nel rosso resta una base
sufficiente a distinguere le sagome nelle ombre.

La [galleria V6](../data/presentation/effects-comparison-20261004-v6/index.html)
permette di scegliere PSX, V4 o V5 come riferimento a sinistra. Solo PSX/V6
sono coppie acquisite nella stessa run; i riferimenti precedenti provengono
dallo stesso checkpoint in run distinte. [Evidenze](mgs-effects-optics-v6-evidence.json).

## Revisione dello stile di tutti gli adattatori — 5 ottobre 2026

Il criterio della revisione è «reimplementazione moderna lato Vulkan, con
resa generale simile all’originale». Sono state riviste individualmente le
60 schede: 27 ritoccate direttamente, 11 aggiornate tramite profili condivisi
e 22 conservate. Il [resoconto per effetto](mgs-effects-style-review.json)
riporta osservazione, decisione, sorgenti con hash e riscontri disponibili.
Le schede dei boss si sovrappongono alle famiglie comuni: non sono 60 effetti
indipendenti né 60 scene convalidate.

I materiali animati usano nuovamente sagoma, palette e fotogramma degli asset
originali: fumo, vampe, sangue, impronte, simboli e bagliori non vengono
uniformati a poche forme procedurali. Il filtro continuo, la profondità e
piccoli contributi di volume/riflesso vivono nella presentazione. Per i
billboard autenticati di `blastoff`, la ricostruzione della geometria ora
conserva texture e UV animati; prima li sostituiva con un dominio sintetico.
Anche vetro e acqua in superficie conservano il disegno del materiale,
inclusi gli stadi di rottura del vetro, sotto una rifrazione più contenuta.

Il termico di gameplay torna a una palette calda coerente con `GoggleIR`,
senza falsocolore arcobaleno. La prova con il vero NVG ha mostrato una doppia
amplificazione: il bloom sommandosi alla palette già intensificata cancellava
il dettaglio della stanza. Il passaggio corretto conserva rumore evidente,
base verde e separazione fra livelli luminosi. Un’asserzione GPU protegge
quest’ultima proprietà. I due visori sono stati equipaggiati tramite input
al menu originale, senza forzare flag o modificare RAM.

Scie, mimetica, duplicati del Ninja, plasma e laser conservano colori e
dissolvenze originali; eliminate le dominanti azzurre e le emissioni residue
arbitrarie. Stun conserva la sequenza bianco/giallo, l’allarme resta più vicino
al blend originale, HIDEO torna a un backplate nero. I monitor non ricevono
uno scorrimento luminoso inventato. Il confronto del frame 7440 ha inoltre
rivelato una dissolvenza troppo lenta: FadeIO sottrae/somma luminosità, mentre
l’adattatore la trattava come trasparenza. Il passaggio corretto rispetta il
nero anticipato delle zone scure; la prova GPU verifica anche questi livelli
intermedi, non soltanto gli estremi. La taratura V6 approvata di acqua, bolle,
scafo, CRT e neve resta invariata.

Passano 359 test in 28 suite, con integrazione Vulkan abilitata, audit delle
77 firme di animazione, layout delle bolle e catalogo. Quicksave e memory card
protetti hanno hash invariati. Sette run rappresentative esercitano 17
famiglie, senza downgrade del renderer. La
[galleria della revisione](../data/presentation/effects-review-20261005-v7/index.html)
contiene 14 coppie PSX/Vulkan dello stesso VBlank, PNG della presentazione a
3840×2160 e tutte le schede consultabili. Il riferimento PSX è il framebuffer
nativo del runtime, non una nuova acquisizione SwanStation.

Questa revisione completa l’esame di sorgenti e adattatori, non la verifica
visiva di ogni variante. Restano da ampliare le scene di specchi, vetri,
granate, fonderia e le combinazioni dei boss. La run disco 2 esercita soltanto
le ombre fra gli effetti classificati e non certifica REX, Liquid o la jeep.
Le primitive prive di asset associabile conservano il ripiego analitico;
il conteggio di una famiglia non dimostra da solo tutti i binding ai PCX.

## CRT condiviso e leggero — revisione 8

La ricerca parte dalla [panoramica ufficiale Libretro](https://docs.libretro.com/shader/crt/)
e dai preset di `libretro/slang-shaders`, fissati al commit
`0b3ff0b240f82f4cb5e5d0117f8f2080e2764cf7`. La documentazione indica
Easymode come popolare e performante e ZFast per dispositivi poco potenti;
non pubblica una classifica quantitativa d'uso. Le fonti, gli hash e le
valutazioni sono raccolti nella [ricerca CRT](mgs-crt-research.json).

| Famiglia | Passaggi del preset esaminato | Indicazione per MGS |
| --- | ---: | --- |
| ZFast CRT, CRT-Pi, FakeLottes | 1 | Fascio e griglia con un solo campione: riferimento per il costo ridotto. |
| CRT-Lottes Fast, Easymode | 1 | Ricostruzione del segnale, fascio e maschere; otto letture nel ramo predefinito esaminato. |
| Easymode Halation | 5 | Separazione fra alone vicino e diffusione ampia. |
| Guest Advanced Fastest | 5 | Versione ridotta della famiglia avanzata. |
| Guest Advanced, Royale | 12 | Riferimenti qualitativi per fosfori, beam e diffusione; il numero di passaggi non equivale a una misura GPU. |

Il nuovo `mgs_crt.glsl` è un'implementazione indipendente, condivisa fra
video ed effetti autenticati. Il fascio si allarga con la luminosità, la
griglia monocromatica dei fosfori è integrata sul footprint del pixel e la
risposta luminosa usa un'approssimazione gamma 2. Alle scale insufficienti,
il raster sfuma verso la propria media per limitare il moiré. Fascio e
griglia non aggiungono texture, attachment, history o passaggi.

La decompilazione conferma lo stesso `ScnMask` a 112 righe alternate in
cinematica rossa/verde, NVG e termico. Tutti e quattro usano il nuovo CRT:
le riprese conservano il segnale intenzionalmente limitato a 320×112,
mentre i visori preservano i dettagli subpixel della scena moderna.
Il bloom cinematico passa da 48 a 8 letture fisse, per 14 letture complessive
invece di 54; rimangono due scale di diffusione e i punti di bianco V6.
NVG e termico restano a nove letture totali con le proprie ottiche.

I due video Codec sono riconosciuti nel buffer dell'attore `menuman.c`
attraverso proprietà complete delle primitive, UV, CLUT, pagina e rettangolo.
Ricevono una risposta CRT locale senza campioni aggiuntivi. La riga mobile,
la commutazione dei ritratti e il crossfade restano originali. La prova ha
anche rivelato un rifiuto dell'opcode PsyQ `SetLineF4` (`0x4c`): il validatore
interpretava il bit 2 come flag texture anche per le linee. Ora rispetta la
famiglia GP0, con regressione su tutti i 32 alias delle linee e VRAM invariata.

Le strisce di acqua/calore, il caricamento delle righe dei titoli, le CLUT,
lo pseudotesto radar e la scala di sintonia non sono raster CRT. Nessun
trattamento globale viene dedotto dalla semplice presenza di righe.
L'inventario resta circoscritto ai sorgenti decompilati e ai layout USA
verificati; eventuali scanline impresse nei bitmap richiedono riscontri propri.

Il benchmark sulla RTX 4060 usa la stessa fixture ScnMask, scala interna
12× e presentazione 3840×2160, due run per versione, 24 frame di riscaldamento
e 96 misurati per profilo. I timestamp includono **l'intero frame di prova**,
copia e presentazione, senza readback: non sono tempi del solo shader.

| Profilo | V7 | V8 | Differenza |
| --- | ---: | ---: | ---: |
| Ripresa rossa | 4,84 ms | 3,19 ms | −34,1% |
| Ripresa verde | 5,08 ms | 3,14 ms | −38,2% |
| Visore notturno | 2,90 ms | 3,24 ms | +0,33 ms |
| Visore termico | 2,75 ms | 3,41 ms | +0,66 ms |

I tempi delle run complete mescolano acqua, zoom e dissolvenze e non
isolano il CRT; per questo il confronto prestazionale usa la fixture.
I risultati non sono una previsione per altre GPU o per l'intera campagna.
Per riprodurre il benchmark, impostare `PS1_RUN_VULKAN_INTEGRATION=1`,
`PS1_VULKAN_HIDDEN_WINDOW=1` e `PS1_MGS_CRT_BENCHMARK_JSON=/percorso/report.json`,
poi eseguire `ps1Runtime_tests` con filtro
`MgsScreenEffectsTest.OptInGpuVisorRetainsSubpixelEdgesAndNativeVram`.
`PS1RECOMP_VULKAN_SHADER_DIR` sceglie la compilazione da confrontare.
Senza la variabile benchmark, il test esegue le normali asserzioni visive.

Passano 360 test in 28 suite, inclusi contrasto delle scanline per i quattro
profili, subpixel dei visori, pausa/rewind, riconoscimento Codec e VRAM nativa.
Dieci run A/B mirate completate senza downgrade producono dieci confronti
visivi; il framebuffer nativo coincide byte per byte fra le due versioni.
La [galleria CRT](../data/presentation/crt-review-20261005-v8/index.html)
consente di confrontare PSX o V7 con V8 e aprire gli originali 4K.
Le [evidenze](mgs-crt-v8-evidence.json) distinguono test, catture e benchmark.

## Fosfori RGB — revisione 9

La griglia monocromatica è sostituita da triadi rosso-verde-blu in tutti
i cinque profili CRT. Ogni triade occupa un pixel del dominio originale:
a 4K sono circa dodici pixel della presentazione, quattro per subpixel.
Il fosforo occupa il 78% della propria terza parte, lasciando un piccolo
intervallo scuro. I tre canali hanno la stessa energia media; la maschera
modula il segnale esistente, perciò una ripresa rossa eccita soprattutto i
fosfori rossi. Il fascio verticale, la diffusione e le dominanti restano
quelli della revisione 8.

L'apertura viene integrata sul footprint del pixel e il contrasto periodico
si attenua quando la risoluzione non basta a distinguere le triadi.
La risposta è calcolata separatamente nei tre canali, senza ulteriori
texture, campioni o passaggi Vulkan. Due benchmark A/B per versione sulla
RTX 4060 non mostrano un incremento distinguibile con sicurezza dalla
variabilità delle run: le differenze medie sono comprese fra −0,11 e
+0,14 ms per frame di prova, non una misura isolata del costo della maschera.

I test GPU verificano i tre picchi cromatici in ordine R/G/B, il raster dei
quattro profili a schermo, il dettaglio subpixel dei visori a parità di fase
del fosforo, la dominante rossa mediata su una triade, pausa/rewind e VRAM.
Passano 360 test in 28 suite. Cinque nuove run e dieci coppie di immagini
confermano framebuffer nativi identici alla V8 e nessun downgrade.

La [galleria RGB](../data/presentation/crt-review-20261005-v9/index.html)
include ritagli 4K ingranditi senza interpolazione. Le anteprime hanno
versioni filtrate per evitare il moiré cromatico introdotto dal
ridimensionamento del browser; gli originali 3840×2160 sono accessibili
separatamente. [Evidenze, hash e misure](mgs-crt-v9-evidence.json).

## Ricostruzione dell'emissione — revisione 10

La V9 modulava il colore già calcolato al frammento con una griglia RGB.
La V10 costruisce invece il colore del frammento dalla luce di celle
campionate a coordinate fisse. La copertura della scena moderna viene
integrata in luce lineare; esposizione, dominante cromatica e rumore
formano il segnale della cella. I tre fosfori RGB hanno posizioni distinte
e un fascio gaussiano che si allarga con l'eccitazione. La loro emissione
e la diffusione ottica si sommano prima della codifica finale del colore.
Non rimane un percorso che mostri l'immagine continua sotto la maschera.

L'ordine segue il principio del [CRT-Royale di Libretro](https://docs.libretro.com/shader/crt_royale/):
linearizzazione, ricostruzione delle scanline, emissione dei fosfori e
diffusione. Royale impiega anch'esso una maschera, ma il bloom viene
ricostruito dall'emissione risultante. Il nostro modello è indipendente
e compatto: non importa il preset completo a dodici passaggi né pretende
di simulare esattamente la fisica di un tubo. Le
[fonti fissate e i limiti del modello](mgs-crt-royale-reconstruction.json)
documentano questa distinzione.

Il percorso finale usa quattro celle adiacenti, con diffusione locale a
supporto compatto, e quattro celle distanti per l'alone ampio. Sono sedici
letture del segnale moderno per frammento, senza nuovi target o passaggi.
I ritratti Codec usano direttamente quattro texel/celle degli indici e
delle palette originali, limitati al rettangolo autenticato; STP e logica
della UI restano originali. La colorazione non viene aggiunta dopo il CRT.

Una regressione GPU confronta due dettagli che occupano metà diverse della
stessa cella e ne eccitano il segnale con la stessa energia. La V10 produce
lo stesso risultato per entrambi; spostando il dettaglio nella cella
successiva l'immagine cambia. La V9 fallisce la prova di equivalenza:
questa verifica distingue la ricostruzione da una griglia sovrapposta.
Restano controllati colore, luminosità, rumore, pausa/rewind e VRAM.

Passano 360 test in 28 suite. Cinque nuove run verificano i profili rosso,
verde, NVG, termico e Codec, senza downgrade e con framebuffer nativi
identici alla V9. Nel benchmark 4K della RTX 4060 il frame di prova completo
richiede mediamente 4,5–5,5 ms, contro 2,9–3,2 ms della V9: circa 1,6–2,6 ms
aggiuntivi secondo il profilo. Queste misure includono copia e presentazione
e due run per variante; non sono tempi isolati del solo effetto.

La [galleria V10](../data/presentation/crt-review-20261005-v10/index.html)
confronta PSX, V9 e ricostruzione dai fosfori, con originali 4K e ritagli
ingranditi. Le [evidenze](mgs-crt-v10-evidence.json) includono la controprova
con il vecchio shader, le misure e gli hash della versione finale.


## CRT V11: interfaccia Codec completa e densità 480p

La griglia condivisa è ora **854×480 celle RGB simulate**, con uscita
3840×2160 e rapporto 16:9. I profili cinematografici rosso/verde, NVG,
termico e Codec usano la stessa densità. La ricostruzione resta basata
sull’emissione delle celle; anche l’impronta delle letture del segnale è
ridimensionata alla nuova cella, senza mantenere il vecchio filtro da due
righe native. Questo non aggiunge dettaglio ai bitmap originali.

Il Codec applica il CRT a ritratti, cornici, pannello centrale, etichette
PTT/MEMORY e cifre della frequenza. Il testo della conversazione resta
fuori dal CRT. `radio.c` compone la UI prima dei due SPRT della scansione
mobile e del testo, nell’ordine inverso della ordering table. Il runtime
riconosce la coppia completa nell’arena del `menuman` vivo e inserisce un
passaggio a schermo intero dopo la scansione e prima del dialogo. Le
primitive originali rimangono; il precedente CRT locale ai ritratti è
rimosso per evitare una seconda elaborazione.

Gruppi parziali, attori non attivi, pacchetti estranei/intercalati e
submission diverse non autorizzano la composizione. Il Codec riusa lo
snapshot già disponibile del presenter: un passaggio e una copia aggiunti
alla UI, senza nuovi attachment. Gli altri quattro profili mantengono
il numero di passaggi precedente.

**362 test in 28 suite superati**, incluse autenticazione, ordine della
composizione, periodicità a 480 righe, posizioni dei tre fosfori RGB,
ricostruzione di sorgenti equivalenti nella stessa cella, pausa/rewind e
VRAM nativa. La controprova con gli shader V10 fallisce i nuovi controlli
di densità, come previsto. Le dieci coppie di catture V10/V11 hanno
framebuffer nativi identici byte per byte; nei due frame Codec anche tutta
la zona del dialogo e della freccia resta identica pixel per pixel.

La [galleria V11](../data/presentation/crt-review-20261005-v11/index.html)
contiene i confronti e i PNG 4K. Le [evidenze V11](mgs-crt-v11-evidence.json)
registrano hash, run, prove e limiti della verifica. Le misure V10 riportate
nella sezione precedente restano storiche: le nuove run rappresentative
richiedono mediamente 7,2–13,7 ms GPU per frame completo sulla macchina di
prova, con 8,35 ms nel Codec; non sono benchmark isolati dell’effetto né
una verifica completa della campagna.


## CRT V12: densità intermedia 426×240

Su indicazione dell’utente la griglia condivisa passa da 854×480 a
**426×240 celle RGB simulate**. Si colloca fra le precedenti 320×112 celle
degli effetti a schermo intero e la densità V11. L’uscita resta 3840×2160.
I cinque profili condividono la nuova densità; il Codec mantiene il CRT
sull’intera interfaccia e compone successivamente il testo della conversazione.
Il modello di emissione/diffusione, il bloom, il numero di passaggi e il
rendering nativo restano invariati. L’impronta di campionamento segue
automaticamente la dimensione delle nuove celle.

I test GPU misurano il periodo di 240 righe, la posizione dei tre fosfori
RGB e l’equivalenza delle sorgenti all’interno della stessa cella. Le
[evidenze V12](mgs-crt-v12-evidence.json) e la
[galleria V12](../data/presentation/crt-review-20261005-v12/index.html)
riportano verifiche e confronti con la versione a 480 righe.

Verifica conclusa: **362 test in 28 suite superati**. Dieci coppie V11/V12
con framebuffer nativo identico; area del dialogo Codec e freccia identiche
nei due frame controllati. Nessun downgrade Vulkan nelle cinque run.


## Revisione V13: granate, profondità delle particelle e riflessi ottici

**La parte sull’esplosione è superata dalla V14.** Il recupero della sagoma
non dimostrava una corretta occlusione rispetto al pannello: la revisione
visiva dell’utente ha individuato questo errore nel criterio di validazione.
La correzione dei riflessi flashbang resta valida.

Verificati in `s15a` il lancio di una flashbang, le fasi di abbagliamento e
recupero e un’esplosione normale visibile dalla camera, tramite soli input.
Le ricette in `scripts/fixtures/mgs-usa-s15a-*.route.json` controllano arma,
munizioni, fase di stordimento e ritorno al gameplay. I checkpoint originali
sono isolati dalle memory card e dai salvataggi dell’utente.

La verifica visiva ha trovato e corretto due difetti:

- Gli otto riflessi di `stngrnd.c` mancavano nel percorso ottico dedicato.
  Viene autenticato l’intero array USA di centri, rettangoli e primitive.
  I riflessi usano il Glow moderno sugli asset originali; essendo effetti
  della camera, non vengono intersecati con la profondità del mondo.
- L’esplosione era tagliata dal pavimento, fino a restare una striscia.
  `AN_Blast_Single` assegna `raise=2000`: questo offset d’ordine era
  conservato solo dalle bolle. Ora tutte le primitive di particelle
  autenticate trasferiscono il valore firmato alla presentazione, e le
  intersezioni morbide lo rispettano nei percorsi con asset e procedurali.
  Geometria, traiettorie e simulazione rimangono originali.

La correzione recupera 120 submission Glow (8×15), mantenendo i 133 fotogrammi
dell’effetto flash. Il confronto dell’esplosione passa da 722 a 2706
pixel di crescita luminosa nella regione misurata, contro 3387 nel frame
nativo: misura della sagoma, non equivalenza pixel per pixel fra camere.

**366 test in 28 suite superati**, comprese regressioni su offset positivo,
nullo e negativo, proprietà delle primitive e profondità dei riflessi.
Due replay controllano RAM/PC per 480 VBlank; i 32 framebuffer nativi
acquisiti prima/dopo e la RAM finale coincidono. Nessun downgrade Vulkan,
errore di gioco o chiamata all’interprete rilevata nelle due run finali.

La [galleria V13](../data/presentation/effects-review-20261005-v13/index.html)
mostra 30 coppie PSX/Vulkan, il prima/dopo e sequenze temporizzate. Le
[evidenze V13](mgs-effects-v13-evidence.json) contengono hash, layout USA,
input e misure. Le sequenze sono campionate ogni 8 o 24 VBlank, non video
alla frequenza reale del renderer. Allarme rosso, mimetica e morte nella
fonderia restano fra i prossimi scenari visivi da verificare.

## Revisione V14: esplosione nello spazio della scena

La V13 sottraeva `raise=2000` alla profondità del billboard: ripristinava
la sagoma luminosa ma portava davanti alla scena anche parti che dovevano
intersecare il pannello. Un offset dell’ordering table non definisce il
volume fisico dell’esplosione. La sola misura dell’area luminosa è quindi
ritirata come criterio di correttezza spaziale.

Il nuovo percorso usa il centro e il rettangolo in espansione di `anime.c`,
autenticati insieme alla firma dell’animazione. Nel checkpoint esaminato il
centro originale è `(2865, 100, -8750)` e il rettangolo misura `3400×3400`.
La trasformazione originale del quad fornisce centro ed estensione nello
spazio della camera; ogni pixel interseca analiticamente un ellissoide e
compone solo il tratto davanti alla superficie della scena. L’atlante
originale continua a determinare sagoma, colore, fase e dissolvenza.

L’emissione cresce con il tratto visibile del volume. Una superficie che
lo attraversa lascia una fiamma parziale; una parete davanti all’intero
volume la occlude anche se `raise` vale 2000. La luce locale usa distanza,
orientamento e colore della superficie, mantenendo i dettagli del pannello.
Si riutilizzano le copie di colore/profondità già disponibili, senza un
passaggio aggiuntivo né ray marching. È una ricostruzione visiva tramite
volume ellissoidale, non una simulazione fluidodinamica.

**367 test in 28 suite superati.** Le regressioni coprono dimensioni vive,
offset d’ordine indipendente, allocazioni non valide e superfici davanti,
dentro e dietro il blast. Il replay di 120 VBlank conserva i testimoni RAM/PC,
la RAM finale e 16 framebuffer nativi. Anche i tre fotogrammi Vulkan verificati
prima/dopo l’effetto sono identici alla V13. La traccia conferma geometria e
atlante originali per tutte le 23 fasi disegnate, con raggio vivo da 500 a 2330.

La [galleria V14](../data/presentation/effects-review-20261005-v14/index.html)
confronta PSX, V13 e V14, con ingrandimento del pannello e sequenza campionata.
Le [evidenze V14](mgs-effects-v14-evidence.json) conservano hash e limiti:
questo scenario non certifica ancora C4, missili, boss e tutte le varianti;
la luce sulla superficie dipende dalla profondità visibile in camera.

## Revisione V15: C4 e mimetica del Ninja

Due ricette input-only verificano eventi originali attraverso le liste GV:
posa/detonazione del C4 in `s15a` e attivazione/disattivazione di `kogaku2.c`
nella cutscene del laboratorio. L’ingresso avanza la storia da 75 a 76 ma
mantiene `s08c`: attendere subito il nome `s08b` era un criterio errato.
I nuovi contatori `actor_<basename>_count` distinguono assenza verificata da
liste incomplete o corrotte, ignorando le vecchie allocazioni scollegate.
Il rapporto TAS ora accetta una ricetta completata prima del limite di frame,
senza accettare registrazioni fisse troncate o ricette fallite.

La prova del Ninja ha rilevato e corretto un arresto reale del renderer:
il materiale di rifrazione, pur riconosciuto, non era dichiarato dinamico
nel percorso delle mesh KMD originali. L’autorizzazione richiede l’attore
vivo e la pagina Direct15 della singola faccia; rivelazione e visione termica
mantengono i materiali ordinari. Il controllo rigoroso del recorder resta
attivo. La rifrazione conserva ora anche l’ampia contrazione orizzontale di
`EQ_VisibleUnit2`, calcolata per pixel sullo sfondo moderno: il solo piccolo
offset delle normali rendeva la sagoma quasi impercettibile.

**386 test C++ e 42 Python superati.** C4: replay di 447 VBlank, 57 framebuffer
nativi e RAM finale identici; tutte le 23 fasi del blast hanno geometria e
atlante originali. I sette scatti Vulkan restano identici dopo la correzione
della mimetica. Ninja: replay di 1375 VBlank, 58 framebuffer nativi e RAM
finale identici, nessuna eccezione o chiamata all’interprete. La regressione
del materiale verifica anche VRAM invariata, facce rivelate, termico e
rimozione dell’attore.

La [galleria V15](../data/presentation/effects-review-20261005-v15/index.html)
contiene 17 coppie PSX/Vulkan, PNG 4K e sequenze campionate. Le
[evidenze V15](mgs-effects-v15-evidence.json) conservano le ricette e i limiti:
il percorso del Ninja registra ancora **sette rifiuti Vulkan per provenienza
incompleta**, da localizzare; non è certificata la presentazione integrale
della transizione. Restano da verificare allarme rosso, caduta nella fonderia,
missili e altre varianti di mimetica/esplosione.
