# Presentazione UHD e rapporto 16:9

Dall'11 settembre 2026 `scripts/run_native.sh` seleziona per impostazione
predefinita la presentazione dalla cache Vulkan in **3840×2160**, con rapporto
**16:9 fisso**. Il gioco si apre a schermo intero; F11 alterna schermo intero
e finestra (inizialmente 1280×720). `PS1_PRESENT_WINDOWED=1` forza l’apertura
in finestra. Ridimensionare o massimizzare la finestra non cambia il target
interno: il quadro viene centrato, ridotto se necessario e circondato da bande
nere quando le proporzioni della finestra differiscono. F9 e F5 mantengono il
loro significato di caricamento e salvataggio.

Non è un ingrandimento del framebuffer PS1. I poligoni sono rasterizzati nel
replay Vulkan a 12×: i 320 pixel orizzontali nativi del molo diventano 3840
colonne reali. L'atlante conserva l'indirizzamento VRAM ed è 12288×6144; la sua
porzione attiva del molo è 3840×2688. La presentazione risolve questa porzione
in 3840×2160, con riduzione verticale e correzione del rapporto pixel originale.
La finestra campiona questo target UHD, non il framebuffer PS1 a bassa risoluzione.
Texture e HUD conservano il dettaglio disponibile negli asset originali.

La simulazione, la VRAM autorevole 1024×512 e il raster di riferimento restano
nativi a **1×**. Il limite del replay accelerato è ora 12×, separato dal limite
della superficie diagnostica software. Non cambia il formato dei savestate.
La cache viene caricata una volta come prima. Un dispositivo incapace di
allocare l'atlante richiesto non viene presentato come compatibile UHD.

## Verifica

365 test C++ e 25 Python passati. La prova di gioco ha mantenuto 27/27 istanze
associate e un solo upload della cache. Il file finale letto dalla GPU misura
3840×2160; verificati anche resize 800×800 e 1200×500.
[Misure e artefatti locali](mgs-uhd-evidence.json).

Il test Vulkan `OptInUhdRasterHasTwelveSubpixelsAndNativeOracle` disegna un
quad spostato di un quarto di pixel PS1: la lettura GPU rileva esattamente tre
colonne UHD di spostamento, con 108 campioni accesi su 144 nella cella esaminata.
Un ingrandimento della cella nativa ne accenderebbe 144. Lo stesso test verifica
il target 3840×2160, l'esecuzione del replay geometrico e l'oracolo nativo a 1×.

I test di geometria verificano inoltre il mantenimento di offset e ritagli del
segnale nel target UHD, oltre all'inquadratura centrata in finestre quadrate,
ultrawide e UHD. Il ridimensionamento agisce esclusivamente sul viewport finale. Prima di ogni
presentazione il renderer confronta il drawable SDL con la swapchain e la
ricrea quando differiscono: non attende soltanto OUT_OF_DATE/SUBOPTIMAL del
driver, per evitare che il compositor Wayland stiri una vecchia superficie.

Con `PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY` il diagnostico scrive ora anche
il target finale `*-scanout.ppm`, oltre alla porzione dell'atlante. Questo
permette di distinguere risoluzione di rasterizzazione, presentazione UHD e
risoluzione fisica della finestra.

Per confronti diagnostici con la presentazione di riferimento:

```sh
PS1_MGS_MESH_REPLAY=0 scripts/run_native.sh
```

Restano i [limiti della scena dalla cache](mgs-original-mesh-replay.md):
verifica principale sul molo, selezione delle facce ancora nativa e assenza di
interpolazione 2×. UHD indica la risoluzione; non certifica prestazioni a 30 fps.
