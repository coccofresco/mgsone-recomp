# Scena sorgente unificata

Il requisito è un unico percorso di presentazione per tutti gli elementi:
geometria dalla cache, stato vivo dalla ricompilazione, materiali e animazione
come proprietà della scena. Non un renderer per le mesh native e un secondo
renderer per correggere singoli oggetti.

## Implementazione del 12 settembre 2026

`mgs_mesh_replay.cpp` prepara un registro per ciascun buffer, identificando
ogni faccia con istanza e indice nella sorgente. `buildSourceFace` ricostruisce
i modelli dalla cache anche quando `DG_OBJ.packs` è nullo. Le istanze invisibili
e i gruppi disattivati dal gioco rimangono esclusi. I modelli articolati,
le seam, gli `extend` e le deformazioni riconosciute usano lo stesso produttore
geometrico. I quad procedurali riconosciuti entrano nello stesso registro usando
il template residente e la forma corrente dell'attore.

`applySourceGeometry` è comune a modelli, facce suddivise, quad procedurali e
facce senza comando nativo. Produce gli stessi riferimenti alla cache, le
coordinate omogenee, il piano vicino e la politica di profondità. Il binding
PCX e il backend Vulkan sono condivisi. La mappa dei pacchetti conserva il ruolo
di adattatore per materiali, ordine e confronto con l'oracolo PS1.

Il recupero delle facce mancanti consuma questo registro; non ricostruisce una
geometria alternativa. Può recuperare una faccia intera con materiale
`TEXT | TRANS | PAINT`, colori persistenti `DG_OBJ.rgbs` e texture risolta dalla
tabella USA `TexSets`. Funziona anche senza `ONEPIECE`. Il culling della sorgente
usa il determinante della faccia e clipping omogeneo in virgola mobile; non usa
SXY interi né l'area quantizzata da NCLIP. UV, CLUT e pagina seguono i materiali
del gioco. Un pacchetto ancora allocato deve concordare con colori e materiale
persistenti; gli override non riconosciuti non autorizzano disegni aggiuntivi.

Prima dell'invio viene controllata l'intera lista effettiva: deve essere finita,
senza cicli e contenere tutti i 257 nodi della OT del buffer esatto. Una faccia
già rappresentata, anche da un figlio della suddivisione, non viene aggiunta.
Un originale accettato dalla trasformazione nativa resta inoltre al suo
adattatore: non si ridisegna il genitore sopra figli parzialmente visibili.

PsyQ `DrawOTag` e DMA usano il medesimo osservatore della presentazione, dentro
la transazione GPU. Le facce aggiuntive entrano nel recorder agli ancoraggi OT,
senza generare GP0 o modificare RAM/VRAM native. Hanno sequenze proprie, nessuna
provenienza GP0/GTE inventata e non alterano i contatori dei comandi PS1.
`sourceRecovery` è metadato host per misurare gli invii effettivi alla GPU;
non cambia l'ABI degli shader né il formato della cache o dei savestate.

Il recupero è attivo insieme a `PS1_MGS_MESH_REPLAY=1`, già predefinito nello
script di avvio. `PS1_MGS_SCENE_RECOVERY=0` disattiva soltanto l'integrazione
delle facce interamente assenti, per confronti diagnostici.

## Verifica

328 test C++ passati, con integrazione Vulkan, cache USA e confronto delle pose
con le istruzioni originali dell'EXE locale. I nuovi casi verificano:

- sorgente subpixel con pacchetti mai allocati, senza mutare RAM o registri;
- geometria identica tra adattatore nativo e faccia recuperata;
- modello articolato e `ONEPIECE` attraverso lo stesso produttore;
- visibilità semantica, frustum, winding e materiali non supportati;
- OT incompleta/ciclica, invalidazione della cattura e reset;
- duplicati nativi, originali suddivisi e override di materiale;
- invio dal DMA e dall'osservatore PsyQ con texture sintetica verificata,
  lasciando la VRAM nativa byte per byte identica;
- sequenze e copertura GP0, guardie del recorder e metadati nel bake Vulkan.

La prova USA dal savestate del molo ha completato 1.400 VBlank dopo il
caricamento, usando lo script di movimento/Codec. I contatori sono cumulativi
degli invii, non numeri di asset distinti:

| Misura | Risultato |
| --- | ---: |
| Istanze associate alla cache | 27 / 27 |
| Facce nel registro sorgente all'ultima cattura | 2.792 |
| Pacchetti nativi adattati alla geometria cache | 175.882 |
| Disegni aggiuntivi realmente inviati a Vulkan | 38.873 |
| Duplicati evitati | 28.632 |
| Materiali non verificati, esclusi dal recupero | 1.620 |
| Frame con cattura incompleta / mutazioni non supportate | 0 / 0 |
| Caricamenti della cache | 1 |
| Byte della cache riletti e verificati dalla GPU | 94.074.416 |

Tre catture della presentazione sono effettivamente 3840×2160. Il frame 97206
mostra il molo, Snake, gli effetti e il radar ed è stato ispezionato visivamente.
Lo slot utente conserva l'hash precedente. Evidenza e percorsi locali in
[mgs-unified-source-scene-evidence.json](mgs-unified-source-scene-evidence.json).
La cattura visiva non è una certificazione pixel per pixel dell'intero gioco.

## Dipendenze ancora da rimuovere

Il produttore geometrico comune è operativo; **l'autonomia completa di tutti
gli elementi non è ancora raggiunta**. Illuminazione dinamica `SHADE`, IR,
environment map, trasparenze e modelli indiretti non possono ancora generare
facce assenti dai pacchetti nativi. Gli effetti riconosciuti usano già la cache,
ma la loro selezione dipende ancora dalla preparazione nativa. HUD e Codec
rimangono operazioni 2D runtime nel backend condiviso, non mesh KMD.

L'ordinamento conserva le 256 fasce OT, compreso `raise`; le facce recuperate
precedono i comandi già presenti nella propria fascia. Non è ancora un
ordinamento autonomo con depth buffer per tutta la scena e non risolve ogni
occlusione tra elementi nella stessa fascia. La suddivisione dei genitori già
rappresentati, i fallback 2D, le famiglie LOD e l'interpolazione restano lavoro
successivo. I 459 prefissi con precisione incompleta nella prova includono
ancora questi fallback; non vanno confusi con frame incompleti.

Le estensioni devono aggiungere dati e adattatori a questo registro/produttore
comune, eliminando progressivamente tali dipendenze. L'assenza di un adattatore
materiale non deve introdurre un'altra copia della ricostruzione geometrica.

## Provenienza dei contratti

Verifica sui sorgenti locali di `FoxdieTeam/mgs_reversing`, commit
`217509eae471629b1a53633b816c61adfef9ed5c`, confrontati con la ricompilazione USA:
`source/libdg/chanl.c`, `sort.c`, `bound.c`, `opack.c`, `text.c` e le routine
di trasformazione/suddivisione. In USA `FindTexture` è a `0x8001E254`,
`TexSets` a `0x800B46C8`; il hook prima di `DG_SortChanl` è a `0x8001E0AC`.
Non sono stati copiati asset commerciali nei file tracciati.
