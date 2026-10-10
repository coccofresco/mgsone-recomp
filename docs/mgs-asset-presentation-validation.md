# Presentazione Vulkan da asset: verifica su GPU dell'8 ottobre 2026

## Regressione delle registrazioni — 10 ottobre 2026

Il primo promo usava anche checkpoint anteriori al fix PCX descritto sotto.
Ripristinarli con un eseguibile corretto non ricrea le risorse già perse:
Mantis aveva 154 texture registrate contro 328 nel donor, Hind 132 contro
345, la jeep 151 contro 396. Il difetto era visibile anche nel framebuffer
originale del runtime. Il controllo delle sole mesh inviate e della durata
del video aveva erroneamente ammesso queste riprese.

Le sequenze sono state rigenerate attraverso il caricamento originale e le
ricette di gameplay già documentate. Gli inventari delle tre scene coincidono
ora con SwanStation per tutti gli ID e i layout PCX, inclusi pagina, palette,
offset e dimensioni. Non sono state inventate associazioni texture o modificate
le mesh per singole inquadrature. Le fixture di danno di Mantis/Hind restano
esplicitamente sintetiche: questa prova non certifica gli scontri completi.

La prevenzione riguarda l'intero percorso di verifica:

- `mgs_scenarios.py` e `mgs_situations.py` confrontano anche l'inventario
  texture. Un vecchio risultato di progressione non basta ad autorizzare
  una continuazione con inventari divergenti. I rapporti storici vengono
  rivalutati dai loro dump RAM, senza riscrivere le evidenze originali.
- La registrazione video usa lo stesso contratto della presentazione su GPU:
  istanze complete, materiali e geometria presenti, cache verificata, GPU
  fisica, validation layer e assenza di errori. Il montaggio rilegge questo
  contratto e gli hash di video, metriche, log e configurazione prima di iniziare.
  I filmati respinti rimangono diagnostici e non sono esportabili dal montatore.
- La luce ambiente della presentazione legge `DG_Ambient` dalla RAM USA
  (`800ADACC`, verificato sulle istruzioni retail) invece dei registri temporanei
  GTE. Gli override ambiente dei singoli oggetti restano validi e la RAM/GTE
  guest non vengono modificati. È un rafforzamento del contratto delle luci:
  nelle sequenze investigate i due valori coincidevano, quindi **non è la
  correzione del lampeggiamento osservato nel vecchio promo**.

Nella ripresa continua di Mantis, 450 fotogrammi, la misura diagnostica degli
impulsi alternati di luminanza passa da 98 campioni sopra soglia a zero dopo
il caricamento completo; il confronto visivo mostra anche librerie, quadri e
materiali prima mancanti. La metrica è descritta nel rapporto e non costituisce
una soglia universale per lampi intenzionali, esplosioni o effetti CRT.

Prove, hash e limiti: [rapporto della regressione](mgs-visual-regression-evidence.json).
Le registrazioni restano locali in `data/media/promo-20261010-corrected`;
i checkpoint difettosi e le prime riprese sono conservati come prove negative.
Questa verifica riguarda le finestre catturate dei due CD, non ogni fotogramma
dell'intero gioco.

Queste prove eseguono la presentazione da KMD/ZMD/PCX originali, con upload
della cache verificato tramite rilettura, invii Vulkan e catture del risultato.
**La matrice su GPU fisica passa: otto scene a 3840×2160 su NVIDIA GeForce
RTX 4060, driver 610.57.04, Vulkan 1.4.341.** Passano inoltre otto cicli di
salvataggio/ripristino e 21 casi mirati che eseguono Vulkan sulla stessa GPU.
Le precedenti prove di progressione headless non dimostravano questa copertura.
Risultati, identità e percorsi degli artefatti sono nel
[rapporto GPU](mgs-physical-gpu-evidence.json). Il
[rapporto software del 7 ottobre](mgs-asset-presentation-evidence.json)
conserva separatamente le prove diagnostiche e le fixture di partenza.

L'estensione successiva con [SSAO da Godot](mgs-ambient-occlusion.md) ha una
matrice hardware e [ricevute proprie](mgs-ambient-occlusion-evidence.json),
confrontate con i frame precedenti riportati qui.

## Risultati su RTX 4060

L'8 ottobre l'accesso ai dispositivi e al display è disponibile. Le prove usano
finestre SDL ordinarie visibili e richiedono esplicitamente una GPU fisica e
`VK_LAYER_KHRONOS_validation`. I rapporti registrano il dispositivo effettivamente
selezionato: tipo discrete, vendor `0x10de`, device `0x2882`. Non è stato usato
un driver software né `VK_EXT_headless_surface`.

| Finestra catturata | VBlank | Risoluzione | Media GPU diagnostica | Esito |
| --- | ---: | --- | ---: | --- |
| Molo | 180 | 3840×2160 | 14,91 ms | Passa |
| Intro subacquea | 180 | 3840×2160 | 12,60 ms | Passa |
| Ingresso tetto `s11h` | 180 | 3840×2160 | 5,25 ms | Passa |
| Anticamera Mantis / Meryl | 180 | 3840×2160 | 6,07 ms | Passa |
| Jeep, primo sbarramento | 180 | 3840×2160 | 12,42 ms | Passa |
| Neve | 180 | 3840×2160 | 22,58 ms | Passa |
| `s17a` | 180 | 3840×2160 | 15,70 ms | Passa |
| Fornace `s13a` | 180 | 3840×2160 | 3,09 ms | Passa |

Sono timestamp della GPU, con diagnostica e catture attive: non sono FPS
misurati senza strumentazione né percentili. Il framebuffer originale resta
320×224. Tutte le scene verificano il caricamento della cache tramite rilettura,
gli invii delle facce originali e la completezza delle istanze visibili, senza
materiali mancanti, downgrade o errori di validazione. In totale sono state
inviate **338.182 occorrenze di facce originali**; non sono modelli unici.

Sono state esaminate le catture centrali delle otto finestre, confrontate con
il framebuffer nativo dello stesso VBlank: composizione, posizione degli actor
e materiali risultano coerenti, senza buchi evidenti dovuti a geometria o
materiali assenti. Le anteprime sono ridotte; gli originali 4K rimangono nelle
sessioni. Confronti: [presentazione GPU](../data/validation/presentation/physical-gpu-20261008/matrix-preview.png)
e [framebuffer originale](../data/validation/presentation/physical-gpu-20261008/native-preview.png).
Non si richiede uguaglianza pixel per pixel agli effetti moderni.

La suite mirata passa **133 test su 135**, con due fixture RAM retail opzionali
escluse. Di questi, **21 sono test di integrazione GPU effettivamente eseguiti**:
illuminazione per pixel, copie e precisione cromatica, profondità e maschere,
PCX a 4/8 bit e palette dinamiche, raster UHD, fog, visori, fade, glow, bolle,
esplosioni e cattura fotografica. Nessun errore del validation layer. I 133 test
comprendono anche controlli CPU: non vanno presentati tutti come prove GPU.

Il controllo di stabilità nello stesso processo supera **8/8 cicli** di pausa,
salvataggio e ripristino. Nei sei cicli misurati, dopo due di riscaldamento,
memoria residente, file aperti e contabilità Vulkan non crescono: RSS
530.898.944 byte, 53 descrittori, 1.755.159.728 byte in 18 allocazioni Vulkan
tracciate, invariati fra primo e ultimo campione. La contabilità esclude le
allocazioni private del driver, swapchain e ImGui. Le memory card personali
e il quicksave sono rimasti identici. Otto cicli non certificano stabilità
per una partita di molte ore.

Questa verifica copre finestre campione del runtime locale sulla RTX 4060.
Restano distinti la validazione dell'intero gioco, Steam Deck/AMD/Intel e
l'esecuzione GPU dei pacchetti AppImage/Flatpak finali.

## Diagnostica software precedente e significato dei risultati

Il 7 ottobre l'ambiente non esponeva una GPU fisica né un display utilizzabile.
Le prove seguenti usavano Vulkan 1.4.354 su Mesa Lavapipe/llvmpipe, LLVM 21.1.8,
con `VK_LAYER_KHRONOS_validation`. Questi risultati storici restano diagnostici
su CPU e non sono misure delle prestazioni su Steam Deck, AMD, Intel o NVIDIA.
Il driver software non è un nuovo requisito del gioco.

`PS1_VULKAN_HEADLESS_SURFACE=1` crea una superficie `VK_EXT_headless_surface`,
usando lo stesso renderer, pipeline, swapchain, asset e readback della finestra
ordinaria. SDL dummy fornisce dimensioni ed eventi. Non abilita `PS1_HEADLESS`
e non disattiva il rendering. Il percorso è diagnostico, esplicito e fallisce
se l'estensione manca. Il lancio normale mantiene la superficie SDL.

Il molo è stato catturato a **3840×2160**, 16:9; il raster della geometria è
3840×2688 e viene risolto nel formato di presentazione. Il framebuffer originale
rimane 320×224. Non è un semplice ingrandimento di quel framebuffer: vengono
verificati gli invii della geometria originale e la risoluzione del readback.
La risoluzione delle texture e dei caratteri originali resta quella degli asset.

La matrice comprende molo, sequenza introduttiva con video verde/subacqueo,
ingresso del tetto `s11h`, Mantis, fuga in jeep, neve, `s17a` e fornace `s13a`.
Sono brevi finestre da checkpoint, non interi livelli o scontri. Il checkpoint
Mantis deriva da una fase predisposta tramite memory card: il caricamento e
l'inizializzazione degli actor sono originali; l'arrivo in quella fase non è
certificato. Le identità dei singoli eseguibili sono conservate nei rapporti:
gli scenari non interessati da un fix non sono stati rieseguiti inutilmente.

| Finestra catturata | Risoluzione verificata | Esito diagnostico su CPU |
| --- | --- | --- |
| Molo | 3840×2160 | Passa |
| Video verde/subacqueo dell'intro | 1280×720 | Passa |
| Ingresso del tetto `s11h` | 1280×720 | Passa dopo ricaricamento corretto |
| Anticamera Mantis / Meryl | 1280×720 | Passa dopo ricaricamento corretto |
| Jeep, primo sbarramento | 1280×720 | Passa dopo ricaricamento corretto |
| Neve | 1280×720 | Passa |
| `s17a` | 1280×720 | Passa |
| Fornace `s13a` | 1280×720 | Passa |

La suite con Vulkan attivo supera **133 test**, con due fixture retail opzionali
escluse e nessun errore del validation layer. La suite runtime ordinaria supera
**1356 test** (31 esclusi, inclusi gli opt-in GPU già esercitati separatamente);
passano **192 test Python**, più il controllo finale dei moduli modificati.
Le suite si sovrappongono: questi totali non vanno sommati come casi unici.

Catture: [molo a 4K](../data/validation/presentation/asset-path-20261007/dock-3840x2160.png),
[ingresso del tetto](../data/validation/presentation/asset-path-20261007/hind-final-preview.png),
[Meryl](../data/validation/presentation/asset-path-20261007/mantis-battle-vulkan-preview.png),
[jeep](../data/validation/presentation/asset-path-20261007/jeep-final-vulkan-preview.png).
Questi file restano locali insieme agli asset originali e alle catture native
dello stesso frame; non sono aggiunti al pacchetto pubblico.

## Difetti trovati e correzioni

### Caricamento rapido che perdeva texture

Il primo controllo della grafica falliva su tetto, Mantis e jeep. Il framebuffer
nativo mostrava già superfici senza texture: non era corretto aggirare il problema
nel binding Vulkan. Sul passaggio retail `s11c` → `s11h`, il percorso rapido
registrava **153** texture, contro **345** con tempi CD originali e nel donor.
Disabilitare la decomp C non cambiava il difetto.

La decomp `libdg/loader.c` mostra che `DG_LoadInitPcx` decomprime la PCX in un
buffer allocato con `GV_AllocMemory2`, carica palette e immagine, poi lo marca
come eliminabile con `GV_FreeMemory2`. La memoria viene normalmente recuperata
nel successivo frame. Nel burst del loader, il buffer temporaneo si esauriva:
le PCX successive fallivano l'inizializzazione senza impedire l'avvio della scena.
Lo stesso numero di settori CD e lo stesso stato di gameplay non rilevavano
questa perdita.

Il binding USA verificato dopo le due chiamate `LoadImage` e il deferred free
rilascia ora **solo il buffer della PCX appena caricata**, tramite l'originale
`GV_FreeMemory`. Opera esclusivamente durante il burst `FS_LoadStageSync`.
Non compatta gli heap, non libera altri buffer e non introduce tick del gioco.
Le due `LoadImage` sono sincrone nella configurazione nativa corrente: tutti i
byte sono consumati prima del rilascio. Questa condizione va rivalutata se si
introducono upload che trattengono puntatori alla RAM guest.

ABI USA: ritorno PCX `800207C0`, stack locale `sp+24`, `GV_Clock=800AE098`,
`GV_FreeMemory=800164DC`. L'installatore controlla l'identità dell'EXE e le
istruzioni delle chiamate upload/free. I ripristini fuori da un burst conservano
il comportamento originale.

Il passaggio al tetto corretto registra 345 texture, con **tutti i 6144 byte della
tabella identici** al percorso temporizzato. Rimane a 545 VBlank complessivi di
percorso, contro 763 del temporizzato: il totale include scala, script e attese,
non misura la sola lettura. La verifica A/B viene inoltre estesa a caricamenti
di entrambi i CD, includendo identità e layout delle texture.

Passano **10 coppie A/B**, nove salvataggi retail e l'ingresso predisposto di
Mantis: **40 intervalli del loader**, ciascuno con gli stessi settori e le stesse
scene. I caricamenti rapidi richiedono **4–6 VBlank**. Tutti e dieci i confronti
dei materiali finali coincidono; Mantis recupera 328 texture e la jeep al primo
sbarramento ne conserva 396. Sono conteggi registrati, non texture tutte visibili
contemporaneamente.

`mgs_loading_check.py` ora confronta i record vivi DG_TEXTURE: ID, palette,
pagina, dimensioni e offset. Esclude solo il byte di uso/residenza e la posizione
nella tabella hash. Un test negativo dimostra che perdere un record o cambiarne
la palette invalida il risultato. Il controllo non certifica ogni byte della
VRAM né ogni altro tipo di risorsa.

**I vecchi savestate con texture mancanti restano incompleti.** Il fix non
inventa i dati assenti durante un ripristino: le prove corrette ricaricano la
scena dal percorso originale. I tentativi falliti sono conservati come evidenza.

### Normali nulle definite negli asset

Il KMD del tetto contiene una faccia di area nulla con normali `(0,0,0)`.
Il validatore la trattava come materiale mancante, rendendo incompleta l'istanza.
Una normale nulla realmente presente nello stream è ora un ingresso valido di
illuminazione ambiente. Lo shader già normalizza con protezione contro zero;
stream assenti, indici fuori limite e trasformazioni non valide restano errori.
Il test dedicato costruisce il medesimo dato sia nell'asset sia nella RAM guest.

### Contratti Vulkan e portabilità degli shader

- Maschere di scrittura coerenti fra gli attachment senza richiedere
  `independentBlend`: eliminato `VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605`.
- Push constant aggiornate per tutti gli stage dichiarati nella stessa range:
  eliminato `VUID-vkCmdPushConstants-offset-01796`.
- Descriptor set 0 collegato anche nei draw nominalmente non texturizzati:
  gli effetti condivisi lo usano comunque. Eliminato
  `VUID-vkCmdDraw-None-08600` nelle copie ritagliate.
- Tre fragment shader texturizzati vengono compilati con `glslc -O0`.
  `-O` e `-Os`, con glslc 2026.3 e questo Lavapipe, causavano un crash JIT
  riproducibile nei test del painter e delle PCX a 8 bit. Gli SPIR-V ottimizzati
  passavano `spirv-val`: non è dimostrato quale compilatore contenga il difetto.
  Gli stessi GLSL con `-O0` passano; il driver continua a compilare gli shader.
  Non sono state alterate le coordinate o le regole di campionamento per
  mascherare il crash. La matrice RTX 4060 sopra usa questi stessi shader:
  i tempi sono misurati, ma non costituiscono un confronto A/B con `-O`.

## Riproduzione e controlli permanenti

Per l'accettazione richiesta, dal terminale desktop con accesso alla GPU:

```sh
cd /home/xander/mgs_recomp
python3 scripts/mgs_gpu_check.py
```

Il comando verifica le fixture, esegue le otto scene a 3840×2160 per 180 VBlank
ciascuna e quindi otto cicli di ripristino (due di riscaldamento, sei misurati).
Usa finestre nascoste; `--show` le rende visibili. Rimuove gli override dei driver
software, richiede un dispositivo Vulkan di tipo integrated/discrete e abilita
esplicitamente il validation layer. Non tenta un'esecuzione software se la GPU
o il display non sono accessibili. Le carte e i salvataggi sono isolati.

Il rapporto è in `data/validation/presentation/physical-gpu-*/report.json`, con
GPU selezionata, vendor/device ID, versione driver, invii originali, catture e
timestamp GPU. Questi tempi comprendono il lavoro diagnostico: non equivalgono
a un benchmark senza acquisizione. Il controllo automatico non sostituisce la
revisione visiva delle catture o la validazione dell'intero titolo.

La sessione del 7 ottobre vedeva dal kernel una RTX 4060 senza poter accedere
ai nodi `/dev/dri`, `/dev/nvidia*` e al display. I tentativi falliti e il rifiuto
esplicito del driver CPU sono conservati in `hardware-20261007`.
Con i permessi disponibili l'8 ottobre, lo stesso comando con `--show` ha
completato le prove hardware in `physical-gpu-20261008`; non è stato necessario
allentare i criteri di accettazione o cambiare il renderer.

`mgs_presentation_check.py` e `mgs_soak.py --vulkan` richiedono ora hardware per
l'accettazione. `--allow-software` permette solo rapporti `diagnostic-only`;
i vecchi rapporti senza telemetria del dispositivo non possono passare il nuovo
controllo hardware. `mgs_tas.py --require-hardware` applica lo stesso vincolo
direttamente alla selezione del dispositivo, prima di creare le pipeline.

Esempio separato di sola diagnosi con un driver software e superficie senza display:

```sh
VK_DRIVER_FILES=/percorso/icd.json \
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
python3 scripts/mgs_tas.py record /tmp/mgs-asset-run \
  --state /percorso/checkpoint.ps1s --config /percorso/runtime.toml \
  --frames 36 --min-game-ticks 3 --period-us 100000 \
  --headless-surface --asset-cache data/presentation/usa-source/assets.mgsc \
  --capture-every 6 --native-every 6 --trace-presentation --timeout 240
python3 scripts/mgs_presentation_check.py /tmp/mgs-asset-run \
  --height 2160 --allow-software --output /tmp/mgs-asset-run-audit.json
```

La configurazione dell'esempio deve richiedere 2160p. Per la GPU fisica usare
`--hidden` o una finestra ordinaria al posto di `--headless-surface`.

L'audit richiede asset riletti e verificati, facce originali realmente inviate
alla GPU, copertura delle istanze visibili, catture non vuote della risoluzione
richiesta, chiusura pulita, assenza di materiali/geometria mancanti, binding
rifiutati, downgrade, errori di profondità e di validazione. Permette il reanchor
iniziale da savestate, riportandone il numero. I totali delle facce sono occorrenze
nei frame, non modelli unici. Le ricevute nuove registrano anche gli hash degli
SPIR-V caricati e della cache scelta esplicitamente.

`mgs_soak.py --vulkan` abilita ora esplicitamente la presentazione da asset,
invece di affidarsi ai soli default del renderer. Sono disponibili gli stessi
cache check e l'opzione `--headless-surface`. Sei ripristini consecutivi del molo
hanno esercitato cache, renderer e contabilità delle allocazioni in un processo.
Una sequenza così breve non prova l'assenza di leak a lungo termine.

Restano necessari l'accettazione su SteamOS e altre GPU, benchmark senza
diagnostica e una copertura visiva più lunga di animazioni, effetti e transizioni.
Nessuno di questi risultati certifica che ogni pixel di tutto il gioco sia già corretto.
