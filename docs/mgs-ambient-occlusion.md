# Occlusione ambientale Vulkan da Godot 4.7.2

L'adattatore usa il nucleo SSAO di **Godot 4.7.2-stable**, ultima versione
stabile verificata l'8 ottobre 2026. L'effetto è attivo di default nella
presentazione MGS da asset e si può disattivare dal launcher, oppure tramite
`game.ambient_occlusion = false` nel TOML. La preferenza si applica al successivo
avvio. I profili precedenti ricevono il nuovo valore predefinito senza cambiare
le altre preferenze e senza riscrittura automatica del file.

## Provenienza e adattamento

- [Versioni ufficiali](https://godotengine.org/download/archive/).
- [SSAO 4.7.2](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/renderer_rd/shaders/effects/ssao.glsl).
- [Filtro SSAO 4.7.2](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/renderer_rd/shaders/effects/ssao_blur.glsl).
- Revisione fissata: `ed1daf0bf001b61586d9930840f2f1394092c079`.

I sorgenti di riferimento invariati, le licenze MIT di Godot e Intel ASSAO e
gli hash sono in `engine/third_party/godot_ssao/`. Il pacchetto distribuisce
entrambe le licenze. Il gioco non collega Godot e non importa risorse del motore.

Sono adattati il pattern di campionamento a disco, lo stimatore di oscuramento
in spazio vista, la riduzione degli aloni, il limite del raggio vicino alla
camera, il riconoscimento dei bordi sensibile alla pendenza, la codifica dei
bordi e il filtro che evita di mescolare superfici disconnesse. L'integrazione
dei buffer, la ricostruzione delle normali e il ricampionamento finale sono
specifici del nostro renderer.

Non è l'intera pipeline Forward+ di Godot: questa versione usa **12 coppie di
campioni** su un piano a metà risoluzione, senza le quattro slice deinterlacciate,
le mipmap della profondità o il conteggio adattivo di campioni dei preset Godot.
Queste differenze sono intenzionali e devono restare esplicite nei confronti.

## Contratto del renderer

1. I draw degli asset originali forniscono la focale e il centro ottico della
   propria area dell'atlante. La camera rimane associata a quell'area, compresi
   i due buffer alternati. Non si inventa una focale se mancano i metadati.
2. Il compute legge la profondità lineare R32 della superficie effettivamente
   visibile, prodotta dal raster Vulkan moderno. Ricostruisce posizione e
   normale geometrica scegliendo il vicino sulla stessa superficie; compensa
   la proiezione orizzontale 16:9 del profilo MGS.
3. Calcolo e filtro scrivono due immagini RGBA8 a metà risoluzione. Sono usati
   solo R/G; RGBA8 evita di richiedere formati storage estesi. A 4K il raster
   della geometria è 3840×2688 e i due buffer AO sono 1920×1344: circa 20 MiB
   complessivi, oltre all'allineamento delle allocazioni Vulkan.
4. La composizione finale effettua un ricampionamento bilaterale e attenua
   il colore. Il framebuffer RGB8 usa la stessa approssimazione gamma 2 degli
   effetti esistenti: `colore × sqrt(visibilità)`. L'atlante persistente resta
   invariato, evitando accumulo fra frame, copie e letture del framebuffer.
5. Nessun dato torna alla simulazione, alla VRAM nativa, alle collisioni, agli
   script o ai tempi del gioco. Pausa e savestate non richiedono una storia AO.
   Il ripristino invalida anche le camere memorizzate; resize e distruzione
   rilasciano i buffer tramite la contabilità Vulkan esistente.

Parametri attuali: raggio base **1.000 unità del mondo MGS**, intensità **2,25**,
potenza **2,0**, oscuramento massimo prima della potenza **0,65**, soglia
angolare **0,06**. Il raggio dipende dalla profondità e dalla focale, non da un
numero fisso di pixel; il limite vicino alla camera continua ad applicarsi.
I campioni sono deterministici: nessun rumore temporale.

La prima taratura (400 / 1,5 / 1,5 / 0,5) risultava troppo tenue e circoscritta.
La seconda taratura ha ampliato il raggio nominale di 2,5 volte e raddoppiato l'intensità,
con una curva di contatto più incisiva. Il minimo teorico del moltiplicatore
RGB passa da circa 0,595 a 0,35, prima dei filtri: non è un abbassamento globale
dell'esposizione. Restano gli stessi 24 campioni, buffer, passaggi e filtri
contro gli aloni. La taratura attuale riduce l'intensità da 3 a 2,25 (-25%);
potenza e limite di oscuramento restano invariati. La percentuale riguarda
il parametro di intensità, prima del limite e dei filtri; non promette una
riduzione uniforme dell'oscuramento in ogni pixel, soprattutto dove cresce
il nuovo raggio.

### Raggio crescente con la profondità: donor N8AO

La funzione usa il principio `screenSpaceRadius` di
[N8AO](https://github.com/N8python/n8ao/blob/0fc15550c98119758e83ed4c2d0e87273bae9ed9/src/EffectShader.js#L202),
con [licenza CC0-1.0](https://github.com/N8python/n8ao/blob/0fc15550c98119758e83ed4c2d0e87273bae9ed9/LICENSE).
La revisione fissata è `0fc15550c98119758e83ed4c2d0e87273bae9ed9`;
sorgente originale, licenza e hash sono in `engine/third_party/n8ao_radius/`.
Il pacchetto include `licenses/N8AO-CC0.txt`.

N8AO misura la distanza fra due punti ricostruiti alla stessa profondità,
separati da un raggio in pixel. Per la nostra camera prospettica la
ricostruzione si semplifica senza altre letture della profondità:

```text
H = altezza nativa della vista × passo verticale della sorgente
Rschermo = 0,16 × H × Z / focaleY
R = clamp(Rschermo, 1.000, 3.000)
Rfinale = R × limite ASSAO vicino alla camera
```

`Z` è la profondità lineare in spazio camera; il raggio sullo sfondo cresce
linearmente con Z fino al massimo. Con H=224 e focaleY=320, R vale 1.000 a
Z=8.000, 1.792 a Z=16.000, 2.688 a Z=24.000 e 3.000 da circa Z=26.786.
Il limite vicino alla camera resta quello precedente. La frazione del 16%
è relativa alla vista, indipendente da scala interna e dimensioni della
finestra; la focale segue la camera effettiva, inclusi gli zoom. Il fattore
ASSAO 0,85 e la riduzione al bordo continuano a restringere la ricerca dei
campioni rispetto a questo raggio nominale.

La normalizzazione sull'altezza, il raggio minimo, il massimo di tre volte
e il mantenimento del limite ASSAO sono scelte del progetto, non valori
copiati da N8AO. Sia la ricerca dei campioni sia la caduta con la distanza
usano **lo stesso raggio finale**: allargare soltanto la ricerca lascerebbe
inattivi i campioni oltre il vecchio raggio. Restano 24 campioni e gli stessi
buffer/passaggi; non vengono importati il kernel o il filtro temporale N8AO.

La [funzione di MXAO](https://guides.martysmods.com/shaders/immerse/mxao/#increase-radius-with-distance)
è il riferimento visivo richiesto. La [licenza iMMERSE](https://github.com/martymcmodding/iMMERSE/blob/main/LICENSE)
richiede autorizzazione per incorporarne il codice: l'adattatore usa il
donor CC0, senza importare sorgenti MXAO. HBAO+ è stato esaminato come altro
possibile riferimento per il raggio sullo sfondo; non è stato incorporato.

Il valore di profondità `1.0` indica contenuto senza superficie 3D risolta.
Testo, HUD, Codec, filmati e fondali con questo valore conservano esattamente
il proprio colore; non diventano occludenti. Le viste diagnostiche della
profondità restano indipendenti dall'AO.

## Limiti e decisioni da conservare nelle regressioni

L'occlusione è applicata alla superficie finale disponibile. Un effetto a
schermo che sostituisce la scena e ne invalida la profondità, per esempio una
ripresa subacquea, viene lasciato invariato. Questo primo adattatore non applica
l'AO prima di quella catena di effetti. Il costo molto basso nella relativa
finestra dell'intro non va presentato come il costo di una scena 3D con AO.

Il risultato attenua il colore già illuminato: **non separa il solo contributo
ambiente dalle luci dirette**, come può fare il materiale Forward+ di Godot.
È una scelta di composizione isolata che preserva i materiali e le copie del
renderer corrente. Un futuro inserimento nel contributo ambiente richiederà
un prepass o dati di illuminazione separati, e confronti espliciti con queste
catture. Non descrivere questo adattatore come illuminazione globale fisica.

Come ogni metodo in spazio schermo, non vede occludenti fuori campo o nascosti.
L'espansione con la profondità è una scelta di resa, non ricostruisce dati
mancanti e non equivale a un raggio fisico costante. Il limite di 3.000 unità
evita una crescita illimitata. Il filtro deve evitare aloni fra piani a
profondità lontane, non oscurare il cielo
e lasciare il testo leggibile.

## Verifica e riproduzione

Le ricevute GPU e i confronti della prima taratura sono raccolti in
`data/validation/presentation/ssao-20261008-final/`. La matrice usa la RTX 4060
con driver 610.57.04, richiede il validation layer e verifica le otto scene
da asset a 3840×2160. Il controllo dei ripristini esegue otto cicli nello stesso
processo, due di riscaldamento e sei misurati.

Passano **8/8 scene**, **8/8 ripristini**, **140 test C++** (inclusi 22 casi GPU,
due fixture RAM opzionali escluse) e **197 test Python**. Il caso GPU dedicato
passa anche con `VK_VALIDATION_VALIDATE_SYNC=1`, senza errori di sincronizzazione.
I sei cicli dopo il riscaldamento non mostrano crescita di RSS, descrittori o
allocazioni Vulkan tracciate.

Nei 24 frame campione, **VRAM visualizzata nativa e atlante Vulkan prima della
composizione sono identici byte per byte** alle catture senza AO. Il calcolo e
il filtro misurano **2,50–3,47 ms GPU** nelle sette finestre geometriche a 4K;
la composizione finale complessiva misura 1,08–1,36 ms. La finestra subacquea
con profondità invalidata resta visivamente invariata ed è esclusa da quel
intervallo. Questi numeri comprendono diagnostica attiva.

[Confronto acceso/spento](../data/validation/presentation/ssao-20261008-final/ao-comparison.png),
[cattura 4K](../data/validation/presentation/ssao-20261008-final/meryl-ao-3840x2160.png),
[ricevute e hash](mgs-ambient-occlusion-evidence.json). I dati retail e le catture
restano locali; la distribuzione contiene solo codice e licenze.

### Revisione del raggio e dell'intensità — 8 ottobre 2026

La seconda taratura (intensità 3, raggio fisso) è verificata separatamente in
`data/validation/presentation/ssao-strong-20261008/`: **8/8 scene a 4K e 8/8
ripristini**, più il test GPU dei contatti con validazione della
sincronizzazione. Il piano della fixture è posto a Z=2.000, con estensione
doppia per conservare la proiezione: l'ostacolo vicino dista 150 unità e quello
lontano 1.650, oltre il nuovo raggio. Restano verificati piano isolato,
esclusione di HUD/schermate 2D, ritorno dell'AO dopo la schermata 2D e VRAM
nativa invariata.

Tutti i **24 frame nativi e i 24 atlanti prima dell'AO sono identici byte per
byte** alla matrice senza AO. I tre confronti affiancati usano il medesimo
checkpoint e frame della taratura precedente, senza ritocchi di luminosità.
Calcolo e filtro misurano 2,24–3,02 ms nelle sette finestre geometriche; questo
intervallo non dimostra un miglioramento prestazionale rispetto alla prima
run, perché sono rimasti uguali algoritmo e numero di campioni e i tempi
risentono delle condizioni della GPU. La finestra subacquea resta esclusa.

[Prima/dopo](../data/validation/presentation/ssao-strong-20261008/ao-comparison.png),
[dettaglio dei personaggi](../data/validation/presentation/ssao-strong-20261008/meryl-detail-comparison.png),
[cattura 4K della seconda taratura](../data/validation/presentation/ssao-strong-20261008/meryl-ao-3840x2160.png),
[ricevute della revisione](mgs-ambient-occlusion-tuning-evidence.json).
La ricevuta della prima taratura resta disponibile come riferimento storico;
i suoi 140 test C++ e 197 test Python non sono attribuiti a questa nuova run.

### Raggio dipendente dalla profondità e intensità -25% — 8 ottobre 2026

La versione attuale è verificata in
`data/validation/presentation/ssao-distance-20261008/`: **8/8 scene a 4K,
8/8 ripristini**, il test GPU esteso a nove fotogrammi per confronto acceso/
spento e **17 test Python del launcher/pacchetto**. Il test GPU usa anche
la validazione della sincronizzazione: nessun errore Vulkan nei log. Dopo
il riscaldamento, RSS, descrittori e allocazioni Vulkan tracciate non crescono.

I **24 frame nativi e i 24 atlanti prima dell'AO** rimangono identici alla
matrice senza AO. Le immagini confrontate mostrano contatti meno pesanti
nell'anticamera con Meryl e occlusione più estesa alla base delle strutture
nell'inquadratura distante dell'hangar s17a. Il compute e il filtro misurano
2,21–2,89 ms GPU nelle sette finestre geometriche; la finestra subacquea senza
profondità valida resta esclusa. Sono misure con diagnostica, non FPS di gioco.

[Confronti prima/dopo](../data/validation/presentation/ssao-distance-20261008/ao-comparison.png),
[hangar a 4K](../data/validation/presentation/ssao-distance-20261008/rex-ao-3840x2160.png),
[Meryl a 4K](../data/validation/presentation/ssao-distance-20261008/meryl-ao-3840x2160.png),
[ricevute e hash della versione attuale](mgs-ambient-occlusion-distance-evidence.json).
Le ricevute delle tarature precedenti restano immutate. La mappa delle licenze
del pacchetto include il donor CC0; questa run non produce una nuova release.

```sh
python3 scripts/mgs_gpu_check.py --show
```

Il test `MgsSourceMeshTest.OptInGpuSsaoContactsRespectDistanceHudAndNativeRaster`
confronta AO attiva/spenta su una superficie KMD originale, un ostacolo vicino,
uno oltre il raggio, un elemento HUD e una schermata 2D. Verifica il ritorno
dell'occlusione dopo la schermata 2D e l'identità della VRAM nativa. Include
la stessa separazione di 1.100 unità a Z=2.000 e Z=16.000: solo la seconda
deve ricevere AO dal raggio espanso. Verifica inoltre la separazione di 8.000
unità a Z=16.000 e il limite di raggio con una separazione di 4.000 a Z=48.000.
Questi ultimi casi devono restare identici all'AO disattivata. Richiede
`PS1_RUN_VULKAN_INTEGRATION=1`; per l'accettazione richiedere anche hardware e
validazione con `PS1_VULKAN_REQUIRE_HARDWARE=1` e
`PS1_VULKAN_REQUIRE_VALIDATION=1`.

I contatori `mgs.ssao.resolves` e `mgs.ssao.dispatches` attestano l'esecuzione;
`mgs.ssao.no_camera` segnala frame senza camera autenticata, compreso il seed
iniziale. L'audit rifiuta un effetto abilitato ma mai eseguito. I timestamp
`host.renderer.vulkan_gpu.mgs_ssao_*` misurano calcolo e filtro; la composizione
bilaterale rientra in `resolve_*`. I tempi con catture/validazione non sono un
benchmark FPS del pacchetto finale.
