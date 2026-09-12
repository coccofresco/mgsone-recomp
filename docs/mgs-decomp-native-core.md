# Decompilazione C e formati originali nella presentazione

Il branch `decomp/native-core`, derivato da `presentation/modern-scene`, avvia
la migrazione verso moduli C della decompilazione come implementazione del
gioco, con le funzioni ricompilate mantenute per le parti non ancora portate.
Questo primo modulo sostituisce quattro ingressi del sistema texture. **La
maggior parte della logica del gioco rimane ancora ricompilata staticamente**:
non è stato completato un port C di attori, GCL e overlay.

## Fonte e contratto USA

Fonte: [FoxdieTeam/mgs_reversing](https://github.com/FoxdieTeam/mgs_reversing),
commit `217509eae471629b1a53633b816c61adfef9ed5c`, file
[`source/libdg/text.c`](https://github.com/FoxdieTeam/mgs_reversing/blob/217509eae471629b1a53633b816c61adfef9ed5c/source/libdg/text.c).
Il progetto ricostruisce Metal Gear Solid Integral per PS1: i sorgenti non sono
un port host direttamente intercambiabile con il nostro eseguibile USA v1.0.
Puntatori, globali, eccezioni regionali e confini delle funzioni vanno adattati.

`engine/ps1Runtime/src/games/mgs_decomp/texture.c` è compilato come **C**.
Mantiene ricerca circolare, inizializzazione e formule dei record `DG_TEX`,
con contesto esplicito, tipi di dimensione fissa e indice del record al posto
del puntatore host. `mgs_decomp.cpp` adatta l'ABI MIPS, compresi gli argomenti
sullo stack, e scrive in RAM attraverso `Memory`. Nessun puntatore host entra
nei dati del gioco o nei savestate.

| Ingresso USA | Modulo | Casi mantenuti nel corpo ricompilato |
| --- | --- | --- |
| `0x8001E254` | `FindTexture` | Argomenti fuori contratto |
| `0x8001E2E8` | `DG_InitTextureSystem` | Ripresa interna da un vecchio savestate |
| `0x8001E310` | `DG_GetTexture` | Texture assente: diagnostica specifica USA |
| `0x8001E378` | `DG_SetTexture` | Tabella piena, assert USA, parametri non coperti |

Tutti gli ingressi lasciano inoltre proseguire il corpo originale quando
`nativeResumePc` segnala una continuazione interna. Il rifiuto avviene prima
di cambiare RAM o registri. Le normali chiamate usano il C per impostazione
predefinita; `PS1_MGS_DECOMP=0` consente il confronto con i corpi statici.
Non viene introdotto un interprete nel percorso del gioco.

`scripts/install_decomp_bridges.py`, eseguito da `build_native.sh`, verifica
l'hash dell'intero eseguibile USA e la presenza univoca dei quattro ingressi
generati prima di installare i collegamenti. L'operazione è idempotente.
La ricerca dei materiali per la scena usa la stessa funzione C di ricerca
delle texture, eliminando una seconda implementazione della tabella.

## Geometria originale residente

Il contenitore `MGSAC003` conserva i KMD/ZMD originali, senza la sezione di
vertici espansi. I metadati `MODL` sono un catalogo: i loro intervalli di
corner sono identificatori logici, non indirizzi di geometria duplicata.
Il vecchio `MGSAC001/002` rimane leggibile, ma il produttore della scena usa
anche in quel caso gli stream originali.

`AssetCache::sourceModel` espone viste controllate su descrittori, SVECTOR
indicizzati, normali, indici delle normali, UV e identificatori texture.
Gestisce KMD individuali e modelli inclusi nei contenitori ZMD con base
condivisa. Stream opzionali assenti hanno offset zero. Il confronto delle
varianti usa gli stream originali, inclusi i legami fra vertici, senza
consultare le copie espanse.

Il produttore comune delle facce legge posizione e legami direttamente dai
SVECTOR. Ogni corner del modello inviato allo shader porta un offset nella
sezione originale `SRCE`; il bit alto identifica il vettore intero originale,
decodificato nello shader e trasformato in virgola mobile. Le strutture CPU
di supporto non sono una seconda geometria GPU. I quad procedurali mantengono
il proprio template, perché non hanno un KMD su disco.

La nuova cache USA contiene 19.340 risorse, 6.695 parti di modello e 18.481
texture. Occupa **63.741.104 byte**, contro i 94.074.416 della v2:
30.333.312 byte di copie espanse eliminati. Le altre sezioni rimangono
identiche byte per byte. Rigenerare dai due CD produce esattamente lo stesso
contenitore ottenuto compattando la cache precedente.

```bash
# Conversione della cache locale già estratta.
python3 scripts/build_mgs_asset_cache.py \
  --from-cache data/presentation/usa/assets.mgsc

# Oppure estrazione completa dalle tracce dati raw, non dai descrittori CUE.
python3 scripts/build_mgs_asset_cache.py \
  --disc1 'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).bin' \
  --disc2 'data/discs/usa/disc2/Metal Gear Solid (USA) (Disc 2) (v1.0).bin'

scripts/run_native.sh
```

Il launcher preferisce `data/presentation/usa-source/assets.mgsc` quando
presente. La presentazione rimane UHD 3840×2160 a 16:9 fisso; simulazione,
VRAM e rendering software originali mantengono il loro formato.

## Verifiche e limiti

Le routine C vengono confrontate con le istruzioni dell'eseguibile USA in
194 chiamate del ciclo texture: inizializzazione, collisioni, ricerca,
lettura e sostituzione delle risorse residenti. L'interprete è usato solo
come oracolo nel test. Si confrontano la RAM persistente, i valori restituiti
definiti dall'ABI e i registri preservati; i 64 byte dei salvataggi temporanei
del chiamato sullo stack e i registri temporanei non sono uscite dell'ABI C.

I test della scena corrompono le copie espanse con NaN e valori errati,
poi le eliminano completamente. Geometria e proiezione rimangono corrette,
anche con due varianti di materiale che condividono la geometria. Il parser
convalida tutti i 6.695 modelli del contenitore USA.

Risultati di build, test, ripresa del gameplay e lettura dei buffer Vulkan
sono registrati in [mgs-decomp-native-core-evidence.json](mgs-decomp-native-core-evidence.json).

La verifica finale passa **334 test pertinenti** (MGS, savestate, replay
moderno, GPU/PsyQ), con oracolo USA e integrazione Vulkan abilitati, più
25 test Python. Il gameplay riprende dal savestate utente per 1.400 VBlank:
27 istanze associate su 27, 2.885 chiamate ai moduli decompilati e 198.181
pacchetti GPU con riferimenti ai vettori originali. La cache è caricata una
volta e riletta integralmente dalla GPU con verifica positiva. Non risultano
frame incompleti né mutazioni non supportate nel replay; l'immagine letta
dalla presentazione è 3840×2160. Il savestate utente conserva il proprio hash.

È stata eseguita anche l'intera suite runtime: 1.162 test passati, tre test
opzionali saltati e un errore estraneo a questa migrazione,
`FileIOTest.OpenFileReturnsValidFd`. Il test si aspetta il descrittore 0,
mentre il codice BIOS già presente dal commit `c937bcb` alloca a partire
da 3. L'errore si riproduce anche eseguendo quel solo test; i relativi file
non sono modificati da questo branch.

Questa modifica rimuove la dipendenza dalle copie espanse della geometria,
ma **non rende già tutta la presentazione indipendente dalla preparazione
PS1**. Materiali dinamici, colori d'illuminazione, ordine, suddivisioni e
selezione di alcuni effetti conservano gli adattatori nativi. Le normali e
gli UV originali sono disponibili nelle viste, ma la nuova illuminazione
non è implementata. PCX mantiene indici e palette decodificati senza perdita;
il binding delle palette vive conserva ancora la semantica del gioco.

La direzione successiva è portare caricamento risorse, attori e animazione
dai sorgenti C e far pubblicare loro risorse e istanze direttamente alla
scena. I corpi statici copriranno le routine non ancora portate e le
differenze USA verificate; SwanStation resta il donor/oracolo indipendente.
Non sono ancora introdotti un sistema LOD universale, interpolazione 2×,
illuminazione moderna o tutti gli overlay del gioco.
