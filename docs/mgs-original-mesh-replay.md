# Scena originale dalla cache Vulkan

Verifica locale del 10 settembre 2026, USA v1.0, molo iniziale. Il percorso
sperimentale disegna geometria KMD/ZMD e texture PCX dalla cache immutabile
Vulkan. Sono associate tutte le 27 istanze del checkpoint, comprese le tre
con deformazione runtime. Il codice ricompilato continua a produrre scena,
pacchetti e framebuffer PS1 di riferimento.

## Avvio

```sh
scripts/run_native.sh
```

Servono build e [cache USA v2](mgs-vulkan-cache.md). La modalità imposta la
presentazione [UHD 3840×2160 a 16:9 fisso](mgs-uhd-presentation.md), PGXP, true
color e profondità ibrida. F9 carica lo slot selezionato; F5 lo salva. Dal giorno
11 settembre è il percorso predefinito; `PS1_MGS_MESH_REPLAY=0` seleziona quello
di riferimento. Una cache assente impedisce di attivare il percorso sostitutivo.

## Geometria e posa

La cattura prima del culling conserva camera, istanze e matrici native. Un
hook all'ingresso USA di `DG_SortChanl` (`0x8001E0AC`) associa i pacchetti alle
definizioni originali dopo trasformazione, illuminazione e suddivisione.
Ogni osservatore riproduce l'istruzione originale verificata dalla ricetta.

Aggiornamento del 12 settembre: le [pose articolate](mgs-modern-pose.md) vengono
ricostruite in virgola mobile dagli operandi originali, con propagazione degli
agganci fra istanze. Le matrici native restano separate per l'oracolo dei pacchetti.

L'identità richiede la definizione completa: topologia, normali, UV, relazioni
parent/extend e posizioni. Il cache ID nativo restringe i candidati, senza
costituire da solo una prova. Sono ammesse modifiche del materiale e del solo
flag di shading. Alias sono equivalenti esclusivamente quando i dati di
geometria effettivamente renderizzati coincidono.

Per `shakemdl.c` vengono verificati l'attore vivo nella lista nativa, il suo
riferimento alla definizione e tutti i punti originali conservati dall'attore.
La deformazione diventa uno spostamento per corner rispetto all'originale
immutabile; nessuna coordinata dello schermo PS1 viene usata come geometria.

Quattro hook nel percorso `DG_SubDivide` (`0x8001A618`, `0x80019F60`,
`0x80019CDC`, `0x80019F48`) conservano la discendenza dei corner: ogni vertice
suddiviso è una combinazione dei quattro corner della faccia originale.
I pesi derivano dalle operazioni native, non da un adattamento ai pixel PS1.

I quad procedurali FT4/GT4 usano un template unitario residente nella sezione
TMPL, con forma e posa dell'attore. Non sono spacciati per modelli su disco.
Questo copre spruzzi, particelle e altri effetti del molo. Gli effetti world
space seguono la correzione 16:9, anche dove il percorso DG_PRIM nativo bypassa
l'hook widescreen dei KMD; gli elementi camera space mantengono il loro framing.

L'associazione richiede inoltre sorgente DMA/GP0, coordinate native, colori,
UV, CLUT e texture page esatti. Una verifica separata della proiezione ammette
al massimo quattro pixel di errore dovuti all'aritmetica nativa. L'oracolo
riproduce saturazione GTE e doppia proiezione dei billboard; Vulkan usa la
proiezione float e clipping omogeneo a Z=1, anche per facce che attraversano
la camera. Il limite GTE H/2 non esclude più queste facce dalla cache.

Il vertex shader legge VRTX o le posizioni indicizzate originali, segue seam
parent e proprietari extend, compone la posa e applica la proiezione float.
Restano overscan USA 58/64 e correzione orizzontale 16:9. Colori e ordinamento
sono quelli della simulazione; il contributo originale non viene disegnato una
seconda volta nella presentazione.

## Texture e Codec

I fragment shader leggono direttamente TEXI e PLTE. La scelta della PCX
verifica pagina, CLUT, dominio UV, finestra texture e contenuto nativo. Le
palette animate a 16 colori sono catturate per draw; gli indici restano nella
cache. Palette statiche a 16/256 colori mantengono RGB555 e bit STP originali.

`StoreImage` era uno stub che inviava il comando senza copiare la risposta GPU
nella RAM guest. Ora drena GPUREAD correttamente, incluso l'ultimo pixel dispari.
Questo ripristina il backup usato da `menu/radiotex.c` per le aree temporanee
del Codec. La prova di chiamata esegue due StoreImage, per 7.168 pixel.

Il vecchio stato utente contiene già texel corrotti in quelle aree. Per la sola
presentazione è ammesso recuperare la PCX originale quando il materiale KMD
nomina esattamente l'asset, tutti i texel fuori dalle due aree Codec coincidono,
esiste almeno un texel verificabile esterno e non esistono varianti ambigue.
Le aree sono [768,1024)×[256,260) e [896,960)×[336,432). Il recupero non modifica
né lo stato utente né la VRAM della simulazione.

HUD, testo e ritratti Codec sono grafica 2D runtime: continuano a essere
composti dal replay Vulkan con i dati nativi. Non vengono contati come modelli
KMD/PCX della scena né sostituiti con un quad 3D arbitrario.

## Risorse e ripristino

La cache v2 occupa 94.074.416 byte, caricati una sola volta. F9 invalida solo
le associazioni transitorie, comprese quelle delle suddivisioni. I vertici
restano di 32 byte; gli uniform per draw sono 448 byte. Il formato savestate
non cambia. La memoria guest non è accessibile dagli shader.

## Verifica

299 test C++ (22 suite, integrazione Vulkan abilitata) e 25 test Python passati.
La cache v2 è stata riletta dalla GPU: tutti i 94.074.416 byte coincidono.
Tre caricamenti consecutivi dello stato v7, seguiti da 600 VBlank, hanno prodotto
174.976 pacchetti: tutti sostituiti per geometria e texture e sottoposti alla GPU,
con un solo upload e nessun rifiuto. Hash del salvataggio utente invariato.

Comandi e misure sintetiche sono conservati in
[mgs-cache-scene-evidence.json](mgs-cache-scene-evidence.json). Gli asset,
i readback e i savestate diagnostici restano esclusi da Git.

La prova stazionaria ha sostituito 34.628/34.628 pacchetti della scena, sia per
geometria sia per texture, tutti sottoposti alla GPU. La prova estesa comprende
movimento nelle quattro direzioni, chiamata Codec, ritorno al gameplay e
geometria che attraversa la camera: 176.866/176.866 pacchetti associati,
senza rifiuti di posa o materiali. I 3.663 FT4 aggiuntivi sono ritratti/interfaccia
Codec e rimangono sul percorso 2D. La misura di submission GPU è 175.944:
l'inizializzazione/ripristino del framebuffer richiede il reanchor del replay,
che non viene conteggiato come disegno dalla cache.

Le immagini sono state ispezionate, ma non costituiscono una prova di uguaglianza
pixel per pixel: float, prospettiva corretta e recupero delle vecchie texture
corrotte producono intenzionalmente differenze rispetto al framebuffer PS1.

## Limiti di questa verifica

È coperta la scena del molo, non l'intero gioco. La selezione delle facce,
l'ordinamento e la suddivisione continuano a dipendere dal codice nativo.
La camera e le matrici radice esterne restano native quantizzate; la composizione
articolata usa ora il percorso float descritto sopra. Non sono
implementati invio indipendente dal culling PS1, famiglie LOD certificate,
selezione del massimo dettaglio o interpolazione 2×. Il costo della presentazione richiede ulteriore ottimizzazione. Le misure
seguenti descrivono la precedente configurazione a 4×, non quella UHD.

Prova isolata a clock normale, senza readback GPU: 240 VBlank in 6,462706 secondi
contro 4 nominali. La correttezza della scena non implica ancora velocità reale a 4×.
