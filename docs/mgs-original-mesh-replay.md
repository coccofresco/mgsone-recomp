# Mesh originali nella presentazione Vulkan

Verifica locale del 10 settembre 2026, USA v1.0, molo iniziale. Il percorso
sperimentale legge i vertici dalla cache immutabile Vulkan e li trasforma e
proietta in float. Il codice ricompilato continua a produrre la scena, i
pacchetti e il framebuffer PS1 di riferimento. Non è ancora una presentazione
indipendente dal culling originale né una selezione automatica del LOD massimo.

## Avvio

Con build e [cache USA](mgs-vulkan-cache.md) presenti:

```sh
PS1_MGS_MESH_REPLAY=1 scripts/run_native.sh
```

La modalità imposta la presentazione a 4×, PGXP, true color e profondità ibrida.
F9 carica lo slot selezionato; F5 lo salva. Il comando ordinario
`scripts/run_native.sh` conserva la presentazione predefinita. La cache deve
essere disponibile: la variabile da sola non crea gli asset.

## Associazione e rendering

La cattura prima del culling conserva camera, istanze e matrici native. Un
secondo hook USA, all'ingresso di `DG_SortChanl` (`0x8001E0AC`), associa i
pacchetti GT4 alle definizioni originali dopo trasformazione, illuminazione e
suddivisione. La ricetta verifica l'istruzione sostituita e ne riproduce l'effetto
con propagazione PGXP. I due buffer nativi mantengono mappe distinte.

L'associazione richiede identità della definizione, proprietà della catena
`extend`, indirizzo DMA e sorgente GP0 esatti, coordinate native, colori, UV,
CLUT e texture page coerenti. Ambiguità, deformazioni non riconosciute e
suddivisioni non associabili conservano il percorso di riferimento. Una verifica
indipendente della proiezione impone uno scarto massimo di quattro pixel nativi;
non viene usata per indovinare l'identità della mesh.

Il vertex shader recupera gli originali da un buffer device-local condiviso.
I corner ordinari usano la sezione float VRTX; i vertici di giunzione seguono
la catena parent e leggono le coordinate indicizzate originali, anche quando
non allineate a quattro byte. Gli extend usano la posa del proprietario primario.
Le matrici provengono ancora dalla simulazione nativa: la loro composizione e
la proiezione nel nuovo percorso sono float. Sono mantenuti la correzione
orizzontale 16:9 e l'overscan verticale USA 58/64.

Il pacchetto sostituito viene disegnato una sola volta nella presentazione con
la geometria originale. UV, colori, materiali, ordinamento, effetti e interfaccia
rimangono quelli prodotti dal gioco; le texture usano ancora la VRAM nativa.
Il calcolo grafico di riferimento continua sotto al cofano. La provenienza GTE
non più valida viene rimossa, conservando la provenienza esatta del trasporto.

La cache non viene ricaricata con F9. Il ripristino invalida le associazioni
transitorie, ricostruite dalla scena successiva. L'ABI dei vertici resta a
32 byte; gli uniform per draw crescono a 320 byte. L'arena di upload può crescere
a fence completata. I PaintPixels consecutivi con uguale sorgente vengono
raggruppati per riga, mantenendo separati i risultati nativi di colore e maschera.

## Evidenza riproducibile locale

- Build `ps1Runtime` e `ps1Runtime_tests`; 263 test C++ passati con integrazione
  Vulkan abilitata, inclusi bake, confronto GPU di colore/maschera/profondità,
  cache MGS, associazioni e savestate. Altri 25 test Python passati.
- Ultima prova dal savestate del molo: 120 VBlank dopo il caricamento,
  23.303 pacchetti sostituiti e altrettanti effettivamente sottoposti alla GPU,
  un solo upload della cache, nessun rifiuto della verifica di posa.
- 27 istanze native, 19 associate: le otto rimanenti comprendono tre definizioni
  deformate, quattro istanze con modifiche a materiali/flag e un'identità ambigua.
- Readback della GPU nei frame 97150, 97170 e 97190: Snake, ambiente, radar,
  acqua ed effetti visibili. È un'ispezione visiva, non una certificazione di
  equivalenza completa del framebuffer.
- Caricato lo stato v6, salvata una copia diagnostica v7 e ricaricata tre volte
  in un nuovo processo, poi eseguiti 600 VBlank. Hash del salvataggio utente
  invariato. Nessuna modifica al formato savestate introdotta da questo percorso.
- Prova con movimento e clock normale: 240 VBlank in 6,640235 secondi contro
  4 secondi nominali, con 48.516 pacchetti originali sottoposti alla GPU.
  Il percorso a 4× non mantiene ancora il tempo reale sulla macchina di prova.

I risultati sono esclusi da Git: `.local/test-mgs-mesh-final.log`,
`.local/test-mgs-mesh-python.log`, `data/native/mesh-replay/final-metrics.json`,
`final-gpu/`, `save-metrics.json`, `reload-metrics.json` e `play-metrics.json`
(nella stessa directory diagnostica). Dischi, cache e savestate restano locali.

## Lavoro restante

La copertura è limitata alle facce inviate dal gioco e alle identità certificate.
Occorre gestire le definizioni dinamiche, separare identità geometrica e varianti
materiali, risolvere le ambiguità e verificare altre scene. La cache contiene le
varianti originali, ma manca la certificazione delle famiglie LOD e la selezione
di quella più dettagliata. Mancano inoltre l'invio della scena completa prima
del culling PS1, materiali letti direttamente dalle PCX, interpolazione 2× e
ottimizzazione sufficiente per la velocità reale. Il percorso resta opt-in.
