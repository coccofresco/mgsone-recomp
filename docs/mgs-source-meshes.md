# Mesh complete dai formati originali

Il branch `presentation/source-meshes`, derivato da `presentation/intro`, estende
il percorso predefinito di `scripts/run_native.sh`. Vulkan disegna le facce
complete KMD/ZMD dalla cache residente, comprese le mesh articolate. Il target
rimane 3840×2160 a 16:9 fisso; simulazione, GPU software e savestate mantengono
il formato nativo.

## Geometria e animazione

Il registro della scena conserva l'identità dell'istanza, del modello e della
faccia originale prima dei test di visibilità PS1. Il medesimo produttore
`buildSourceFace` serve sia la presentazione completa sia l'adattatore precedente:
non esiste una seconda copia espansa della geometria. Gli indici originali
individuano gli `SVECTOR` letti dal vertex shader nel buffer della cache MGSAC003.

La posa in virgola mobile compone la gerarchia, gli agganci fra oggetti e le
articolazioni. Le giunzioni fra parti risolvono il vertice originale del padre;
le parti `extend` usano la trasformazione del proprietario. Le deformazioni
runtime sono spostamenti rispetto ai vertici originali. È stato rimosso il
limite di 24 articolazioni nell'adattatore della presentazione: rimangono i
controlli sulle dimensioni dei dati e sulla validità della gerarchia.

Una faccia viene disegnata una sola volta, anche quando la PS1 la suddivide in
molti pacchetti. La sostituzione occupa il posto del primo figlio nell'ordine
di composizione. I successivi figli continuano a essere eseguiti dalla GPU
software, ma diventano operazioni vuote nella presentazione. Le facce assenti
dai pacchetti sono disponibili dal registro originale.

Frustum, verso della faccia e piano vicino vengono valutati dalla geometria
in virgola mobile; i modelli a doppia faccia conservano il proprio flag.
Allocazione dei pacchetti, saturazione SXY e culling GTE non determinano più
la visibilità di queste mesh. Gli opachi usano il test di profondità `LEQUAL`
anche nel profilo Hybrid. Trasparenze ed effetti conservano per ora l'ordine
di composizione del gioco.

La lista nativa deve essere integra e riferita al buffer corretto. Prima di
sopprimere qualsiasi figlio si verificano identità e attualità dei pacchetti e
si risolve il materiale. Una modifica tardiva di un figlio invalida la
sostituzione dell'intera faccia. Il confronto delle coordinate native in
questo controllo verifica l'attualità della cattura, non decide la posizione
o la visibilità della mesh moderna.
Le facce assenti vengono reinserite solo per una nuova preparazione DG della
scena: il riutilizzo della lista durante la pausa Codec non può ridisegnare
una vecchia scena sopra l'interfaccia.

## Materiali originali e superfici generate

`mgs_source_material.cpp` legge UV e normali dal modello originale. Per i
materiali ordinari mantiene le coordinate UV frazionarie; palette, luci e
modifiche del materiale sono stato vivo dell'attore. L'illuminazione SHADE
trasforma le normali con la posa moderna e applica il modello di luci di
`DG_ShadeChanl` in virgola mobile, senza scratchpad e troncamenti intermedi
del GTE. Il colore finale resta RGB8. Le giunzioni indirette seguono la
provenienza del colore nel modello padre.

I materiali dipinti mantengono i colori dell'attore; trasparenza, STP, maschera
e texture window attraversano lo stesso percorso. `IRTEXTURE` indica che un
oggetto può cambiare palette con il visore termico: non disabilita
l'illuminazione ordinaria e non rende necessari i pacchetti. Gli override
attivi di palette e gli UV speciali possono ancora provenire dall'attore.

Due casi dell'intro richiedono una semantica distinta:

- `tex_scrl.c` ruota le prime `altezza-1` righe e lascia fissa l'ultima. Il
  binding prova la permutazione confrontando **tutti** i texel, insieme
  all'identità logica della risorsa. Lo shader applica la fase leggendo la PCX
  immutabile. La fase è ricostruibile anche dopo F9, senza un orologio host.
- `optxtscn.c` sostituisce i materiali segnaposto `a91d/a91e` con due schermi
  128×70 composti tramite copie dei glifi `ef71` nella superficie `cc22`.
  Il bridge verifica i riferimenti `DG_TEX`, pagina, palette e rettangoli
  previsti dall'attore. La mesh resta KMD originale; il materiale usa la
  superficie aggiornabile del replay, con le sue dipendenze di lettura.
  Queste scritte non sono una PCX statica presente sul disco.

Il materiale zero presente in alcune facce delle guardie ha la semantica
`DG_NullTexture`/`EmptyTex`: pagina, CLUT e coordinate intere zero. Non è una
PCX mancante. Il bridge ricostruisce questo materiale anche senza pacchetti
nativi, conservando la lettura definita dal gioco nella superficie runtime.

Le palette PCX animate a 4 bit possono contenere da 1 a 16 colori effettivi;
quelle a 8 bit usano fino a 256 colori. Il binding conserva l'indice originale
e passa allo shader la palette corrente;
un cambiamento dei texel non viene accettato come semplice animazione della
palette. Il percorso statico conserva inoltre la prova completa del contenuto
della texture e la gestione già verificata delle aree temporaneamente usate
dal Codec.

## Verifiche e riproduzione

I risultati, i percorsi delle acquisizioni e gli hash sono in
[mgs-source-meshes-evidence.json](mgs-source-meshes-evidence.json).
Le acquisizioni sono letture della GPU, non immagini ottenute ingrandendo il
framebuffer software. Il contatore `mgs.source.gpu_full_faces` viene aggiornato
solo dopo una submission Vulkan riuscita: preparare la geometria sulla CPU
non basta a incrementarlo.

La selezione di 264 test comprende integrazione Vulkan, cache originale USA,
oracolo delle istruzioni USA, ricostruzione delle pose, sostituzione delle
suddivisioni, maschere, materiali animati e dipendenze del replay. Un confronto
diretto verifica che gli stessi comandi producano una VRAM software identica
con e senza la nuova presentazione. Un altro verifica una gerarchia di 32
articolazioni. La scena con normali animate viene preparata anche senza
allocare pacchetti nativi e senza modificare RAM o registri del gioco.

La build definitiva è stata provata sull'intro completa fino al menu e sul
salvataggio del molo, includendo movimento, apertura del Codec e ritorno al
gioco. In entrambe le esecuzioni sono zero i casi di geometria o materiale
mancante, associazione non risolta, pacchetto modificato dopo la cattura e
rifiuto del replay Vulkan.

| Esecuzione | VBlank | Acquisizioni 3840×2160 | Facce complete inviate a Vulkan | Istanze visibili preparate integralmente |
| --- | ---: | ---: | ---: | ---: |
| Intro fino al menu | 15.600 | 13 | 1.669.714 | 32.629 / 32.629 |
| Molo, movimento e Codec | 1.400 dopo F9 | 5 | 125.248 | 6.094 / 6.094 |

I contatori sommano le occorrenze nei fotogrammi, non il numero di asset unici.
Nel molo sono associate tutte le 27 istanze: nell'ultima cattura 22 sono
visibili, tutte complete; quattro gerarchie articolate comprendono 64 parti
e tre agganci fra oggetti. La cache GPU da 63.741.104 byte è stata riletta e
verificata. L'intro usa VBlank accelerati per copertura, il molo la cadenza
NTSC ordinaria; queste prove non costituiscono un benchmark di framerate.
Le inizializzazioni della superficie nativa all'avvio e al caricamento del
salvataggio restano le operazioni previste, distinte dai rifiuti del replay.

```bash
scripts/run_native.sh

# Solo per confrontare la presentazione con l'adattatore precedente:
PS1_MGS_SOURCE_MESHES=0 scripts/run_native.sh

# Test con cache e istruzioni originali USA disponibili localmente:
PS1_RUN_VULKAN_INTEGRATION=1 PS1_VULKAN_HIDDEN_WINDOW=1 \
PS1_TEST_MGS_ASSET_CACHE=data/presentation/usa-source/assets.mgsc \
PS1_TEST_MGS_USA_EXE=data/assets/usa/disc1/SLUS_005.94 \
build/ps1Test/ps1Runtime_tests \
  --gtest_filter='DmaTest.*:*Mgs*:GpuModernReplayCaptureTest.*:GpuModernReplayPreflightTest.*:GpuModernReplayVulkanBakeTest.*:GpuModernReplayVulkanCopyTest.*:GpuModernReplayVulkanDepthTest.*:GpuModernReplayRecorderTest.*:GpuModernReplayExecutionPlanTest.*:PsyqGpuTest.*:GpuCommandTest.*'
```

## Confini del risultato

Il nuovo percorso elimina i limiti geometrici citati per le mesh riconosciute.
Non aggiunge dettaglio alle texture bitmap originali e non genera nuovi LOD.
La decompressione OAR e la temporizzazione dell'animazione restano nel gioco
nativo; gli operandi dell'animazione alimentano la gerarchia moderna. Radice,
camera e alcuni materiali speciali ricevono ancora valori prodotti dal gioco.
Interpolazione 2×, illuminazione per pixel e ordinamento interamente autonomo
delle trasparenze sono lavori successivi.

La copertura dell'intro e del molo non certifica gli overlay e le scene del
resto del gioco. I quad degli effetti, HUD e Codec conservano i percorsi
dedicati già presenti: non vengono spacciati per mesh KMD. Le acquisizioni
visive non costituiscono un confronto pixel per pixel con SwanStation;
quest'ultimo resta il donor/oracolo separato. Non viene dichiarata una
reimplementazione C completa degli attori.

Riferimenti locali letti: FoxdieTeam/mgs_reversing al commit
`217509eae471629b1a53633b816c61adfef9ed5c`, in particolare `source/libdg/screen.c`,
`trans.c`, `opack.c`, `shade.c`, `loader.c` e `source/takabe/tex_scrl.c`,
`optxtscn.c`. Gli indirizzi del bridge sono quelli USA già verificati nel
progetto; gli indirizzi nel nome delle funzioni Integral non vengono copiati
come indirizzi USA.
