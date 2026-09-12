# Pose della presentazione in virgola mobile

Dal 12 settembre 2026 il percorso cache ricostruisce le pose articolate dagli
operandi di animazione USA, prima del culling, usando composizioni in `double`
e matrici `float` per Vulkan. Non accumula più gli arrotondamenti Q12 e delle
traslazioni intere delle matrici delle ossa. La modalità è predefinita in
`scripts/run_native.sh`, insieme alla presentazione UHD 3840×2160 a 16:9 fisso.

Per confrontare lo stesso renderer con le precedenti matrici native:

```sh
PS1_MGS_PRECISE_POSE=0 scripts/run_native.sh
```

## Operandi e collegamenti

`mgs_pose.cpp` riproduce la struttura di `DG_ApplyRots`, `DG_ApplyMovs` e
`DG_ScreenModelsSingle`. Il riferimento è
[FoxdieTeam, libdg/screen.c, commit 217509e](https://github.com/FoxdieTeam/mgs_reversing/blob/217509eae471629b1a53633b816c61adfef9ed5c/source/libdg/screen.c),
con riscontro sulle istruzioni dell'eseguibile locale USA v1.0:

- `DG_ApplyRots`: `0x8001D170`; `RotMatrixZYX_gte`: `0x80096A44`.
- `DG_ApplyMovs`: `0x8001D030`.
- Rotazioni `Rz * Ry * Rx`, traslazioni KMD e gerarchia `parent`.
- La seconda articolazione usa la base separata del bacino, anche quando la
  prima riceve aggiustamenti. Gli aggiustamenti Z/X/Y ruotano la matrice già
  composta e conservano la traslazione; la trasformazione dell'oggetto viene
  applicata alla fine del percorso con aggiustamenti.
- Le pose `movs` condividono la rotazione dell'oggetto ed ereditano la sola
  traslazione del genitore. `ONEPIECE` conserva la semantica dei grandi scenari.

Gli agganci fra istanze seguono esclusivamente un indirizzo `root` che nomina
una matrice di modello primaria nella stessa cattura. Si verifica anche che
la matrice nativa copiata coincida con quella del proprietario: un riferimento
alla posa del frame precedente non eredita la posa nuova. Le dipendenze vengono
risolte anche con coda riordinata. Cicli, identità duplicate, descrittori estranei,
genitori successivi e intervalli RAM invalidi non pubblicano pose parziali.

Gli originali delle mesh restano nella cache immutabile. Seam e pezzi `extend`
usano la trasformazione precisa del loro proprietario; le deformazioni già
riconosciute continuano a essere spostamenti rispetto ai vertici originali.

## Separazione dall'oracolo

Ogni modello conserva sia la matrice nativa sia quella della presentazione.
Il controllo dei pacchetti DMA/GP0 e la soglia di quattro pixel continuano a
usare la posa nativa. Le coordinate per Vulkan e gli uniform della geometria
originale usano la posa ricostruita. Anche le facce suddivise mantengono le due
serie di coordinate: la nuova posa non viene respinta perché differisce dai
pixel PS1, né viene allargata la tolleranza dell'oracolo.

La ricostruzione legge solamente la RAM. Non modifica GTE, stato CPU, clock,
animazioni della simulazione o formato dei savestate. Le catture vengono
ricostruite dopo F9, senza conservare puntatori alla RAM per il renderer.

## Verifica

308 test C++ passati, in 23 suite, con integrazione Vulkan e cache USA locale.
Il test delle routine USA esegue le istruzioni originali in un interprete usato
**solo dal test**, per 160 pose deterministiche: rotazioni, bacino,
aggiustamenti, loro combinazione e movimenti. Lo scarto massimo osservato è
0,002772 sui coefficienti di rotazione e 5,832160 unità sulle traslazioni.
Queste sono misure della quantizzazione rimossa, non tolleranze del renderer.
Test dedicati controllano le frazioni recuperate, l'assenza di scritture guest,
gli agganci, le gerarchie invalide e la separazione dell'oracolo sulle suddivisioni.

In una prova del molo con 1.400 VBlank, movimento nelle quattro direzioni,
Codec e ritorno al gameplay:

- tutte le 27 istanze sono associate alla cache;
- quattro scheletri da 16 articolazioni vengono ricostruiti, con tre agganci;
- 176.018 pacchetti 3D sono associati, sostituiti e sottoposti alla GPU;
- 78.062 di questi usano pose articolate precise o loro agganci;
- nessun input di posa invalido e nessun rifiuto dell'oracolo di proiezione;
- 3.663 pacchetti dell'interfaccia Codec restano grafica 2D runtime;
- la cache viene caricata una volta; lo slot utente mantiene il proprio hash.

La verifica finale, con rilettura integrale della cache dalla GPU, conferma
94.074.416 byte identici agli originali caricati, 27 istanze associate, 64
articolazioni e tre agganci. Registra 289.341 pacchetti sostituiti, dei quali
288.712 sottoposti alla GPU e 144.674 associati a pose precise. I 629 pacchetti
di differenza ricadono nelle ricostruzioni del framebuffer del replay.
Anche qui non ci sono input invalidi o rifiuti dell'oracolo. I due conteggi
appartengono a esecuzioni distinte: la sequenza di intervalli di input non
garantisce la stessa conversazione Codec o lo stesso percorso temporale.

Le immagini di scanout 4K sono state lette dalla GPU e ispezionate. Comandi e
misure sono in [mgs-modern-pose-evidence.json](mgs-modern-pose-evidence.json).
Gli eseguibili, le immagini e il checkpoint rimangono locali, esclusi da Git.
Un confronto fra immagini di esecuzioni separate non ha dato framebuffer
nativi identici a parità di etichetta VBlank; non viene quindi usato come prova
di isolamento pixel per pixel. Per questo valgono i controlli senza scritture
guest e il confronto con le istruzioni originali descritti sopra.

## Limiti ancora da rimuovere

La matrice radice esterna e la camera rimangono ingressi nativi quantizzati;
gli angoli di animazione conservano i passi originali. Questo intervento
rimuove la quantizzazione della composizione articolata, senza inventare
fotogrammi intermedi. Non aggiunge interpolazione 2×.

Invio delle facce, culling, ordinamento, suddivisione e shading dipendono ancora
dal percorso nativo. Texture e modelli conservano il dettaglio degli asset
originali; selezione di famiglie LOD, illuminazione moderna e culling autonomo
restano da implementare. La verifica di gameplay copre il molo, non gli scenari
successivi. Questa prova con readback e diagnostica non certifica le prestazioni.
