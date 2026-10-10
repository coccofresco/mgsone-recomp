# Caricamenti nativi rapidi — USA v1.0

Il profilo MGS abilita ora `game.fast_loading = true` per impostazione
predefinita. La lettura degli asset di scena perde le attese del lettore CD;
la registrazione delle risorse usa ancora le routine originali ricompilate.
Non vengono accelerati VBlank, timer, simulazione, audio o filmati.

Build verificata: `d38c509e2c6328259e24b427ff826f2b6ef7856a1829012570def64a2f89476d`.
Prove e identità: [rapporto completo](mgs-fast-loading-evidence.json),
`data/scenarios/fast-loading-20261006/result.json`.

## Risultati misurati

**Aggiornamento 7 ottobre:** le misure sotto sono storiche e verificavano
progressione e settori, non la completezza dei materiali. I test della
[presentazione da asset](mgs-asset-presentation-validation.md) hanno trovato e
corretto l'esaurimento della memoria temporanea PCX nei burst. Il confronto A/B
ora richiede anche gli stessi ID e layout texture. Le precedenti prove non vanno
usate da sole per certificare la grafica; i savestate già incompleti richiedono
una ricarica originale della scena.

Passano **24 salvataggi retail Normal**, sui **20 scenari** dei due CD già
presenti nel catalogo. Ogni prova parte da un nuovo processo, carica dal menu
originale e verifica controllo stabile e abbassarsi/rialzarsi. Non certifica
lo scontro completo o la continuità dell'intera campagna.

I **96 intervalli del loader** attraversati richiedono **4–7 VBlank**, circa
**67–117 ms a 60 Hz**. Otto scenari hanno anche una prova A/B con lo stesso
eseguibile, carte, percorso di input adattivo e configurazione, variando solo
`fast_loading`: tutti i 32 caricamenti confrontati leggono lo stesso numero
di settori degli stessi archivi di scena.

| Scena | Tempi CD, VBlank | Percorso rapido, VBlank | Durata nominale prima → dopo |
| --- | ---: | ---: | ---: |
| Eliporto, `s01a` | 246 | 4 | 4,10 s → 67 ms |
| Ocelot, `s04b` | 224 | 4 | 3,73 s → 67 ms |
| `s08c`, storia 111 | 165 | 4 | 2,75 s → 67 ms |
| `s12b` | 205 | 4 | 3,42 s → 67 ms |
| `s13a` | 232 | 6 | 3,87 s → 100 ms |
| `s15a` | 162 | 6 | 2,70 s → 100 ms |
| REX, `s16d` | 246 | 6 | 4,10 s → 100 ms |
| Liquid, `s18a` | 243 | 6 | 4,05 s → 100 ms |

La riduzione negli otto caricamenti di gameplay confrontati è circa
**96–98%**. Le prove sono headless accelerate, con misurazione della vita
dell'actor originale `loader.c` a ogni VBlank. I millisecondi sono la
conversione a 60 Hz, non una misura della latenza fisica su monitor in Vulkan
4K. La scansione ISO iniziale e le attese di script, dissolvenze, mission log
e conferma dei menu restano distinte dalla durata del loader.

## Contratto del percorso rapido

`VirtualFs` risolve gli extent dei file dal CD montato. La lista comprende
solo `/MGS/STAGE.DIR`, `/MGS/RADIO.DAT` e `/MGS/FACE.DAT`. Ogni cambio disco o
ripristino del supporto ricostruisce gli extent; un mount fallito conserva
supporto e policy precedenti. Non esistono intervalli LBA retail codificati
a mano. Un file assente o un LBA esterno agli extent ammessi non riceve
l'accelerazione.

Il controller elimina il termine temporale di seek/settore soltanto quando
la lettura corrente è autorizzata. Conserva risposta ai comandi, INT1,
callback e DMA originali e non consegna il settore successivo prima dell'ACK.
Il credito CD viene azzerato dopo il settore rapido: non può essere
trasferito a un flusso temporizzato adiacente. La modalità XA esclude sempre
l'accelerazione. `DEMO.DAT`, `VOX.DAT`, `ZMOVIE.STR` e `BRF.DAT` rimangono
temporizzati, così come l'inizializzazione ISO e la tabella iniziale di
`STAGE.DIR` fuori dal loader.

**La sola rimozione delle attese non è sufficiente.** `cdstage.c` riutilizza
i buffer temporanei dei DAR resident/nocache. Un primo prototipo che
consegnava tutto tramite le callback CD, senza intercalare il consumatore,
si è bloccato all'avvio: il produttore superava `LoadStageFiles` e riutilizzava
memoria ancora necessaria. Il risultato fallito resta nel rapporto come
regressione da non reintrodurre.

L'adattatore definitivo opera dentro `FS_LoadStageSync`:

1. Esegue l'originale `LoadStageFiles` sui dati già disponibili.
   Durante ogni inizializzazione PCX, dopo gli upload sincroni, recupera il solo
   buffer temporaneo marcato dal gioco come eliminabile (`GV_FreeMemory`).
2. Consegna al massimo una transazione CD/DMA attraverso le callback originali.
3. Ripete consumo e consegna, con limite di 4.096 settori per chiamata.
4. Restituisce il risultato originale di avanzamento, includendo
   `CDBIOS_ReadSync`. Distruzione dell'actor, completamento del loader e
   avvio degli script rimangono al gioco.

Fuori da questo ambito, `STAGE.DIR` conserva la temporizzazione CD. La
routine che consuma risorse impedisce la rientranza delle callback: non
esegue nuovi tick del gioco e non sospende una continuazione nativa nel
mezzo dell'adattatore. I rientri interni da un vecchio savestate proseguono
nel corpo ricompilato originale.

Le ABI USA sono `FS_LoadStageSync = 80024A64`,
`LoadStageFiles = 800248A0`, `CDBIOS_ReadSync = 80023A20`.
`install_decomp_bridges.py` verifica SHA-256 dell'eseguibile retail e le due
istruzioni di chiamata prima di installare il binding. Gli indirizzi della
decompilazione Integral non vengono trapiantati nella versione USA.
Riferimenti locali: FoxdieTeam `217509eae471629b1a53633b816c61adfef9ed5c`,
`source/libfs/cdstage.c`, `cdbios.c`, `file.cnf` e `source/game/loader.c`.

## Regressioni e limiti

- **1.304 test runtime passati**, 31 esclusi, sia nella build ordinaria sia
  sotto ASan/UBSan; **141 test Python** passati. I contratti nuovi coprono
  buffer riutilizzati, budget, registri preservati, clock immobile,
  autorizzazioni degli extent, confini asset/stream, ACK, XA e snapshot.
- **Cambio disco reale `s12c → s13a`:** 504 frame registrati e replay esatto,
  un cambio supporto e una validazione originale, inventario conservato e
  movimento controllato. Nessuna pressione di Start per cambiare disco.
- **Codec:** apertura/chiusura confrontate con SwanStation e replay esatto
  di 146 frame. Questa sola prova di menu non legge nuovi ritratti.
  La ripartenza dal salvataggio di gioco completato attraversa invece il
  Codec obbligatorio del molo e accelera effettivamente **75 settori FACE**
  e **1 settore RADIO**, oltre ai 1.476 settori di scena.
- **Entrata in un filmato dopo caricamento rapido:** la fase sintetica
  `s08b/6137`, derivata dal ramo originale, completa **388 frame**,
  **77.600 macroblocchi** e **1.145.424 frame PCM stereo** consumati dalla
  SPU. I conteggi rispettano i limiti ricavati dal CD e l'endpoint concorda
  con il donor storico. Nessun troncamento MDEC o credito CD perso.
- **Replay storico da NSV10:** 1.460 frame identici, senza
  desincronizzazione della RAM/continuazione, nella porzione di filmato
  già in corso. È una prova distinta dall'entrata appena descritta.
- Le esecuzioni riuscite non registrano errori di gioco o interprete.
  Savestate e memory card personali mantengono gli hash precedenti.

Non è stata aggiunta una nuova certificazione visiva Vulkan. I test
strumentati non coprono ogni funzione del gioco generata; LeakSanitizer è
disabilitato nell'ambiente corrente e resta la limitazione ASan relativa a
`makecontext/swapcontext`.

La policy è host e non cambia il formato del savestate. Per un confronto
con il donor o un vecchio TAS che attraversa caricamenti:

```toml
[game]
profile = "mgs_usa_v1_0"
fast_loading = false
```

Con `true`, i TAS basati su frame assoluti devono essere registrati nuovamente
attraverso i caricamenti. Le macro basate sulle condizioni del gioco si
adattano automaticamente. Il confronto storico richiede comunque le normali
verifiche di identità della build/configurazione.

## Riproduzione delle misure

Usare due copie della configurazione che differiscano soltanto per
`game.fast_loading`, e destinazioni nuove:

```bash
python3 scripts/mgs_scenarios.py run data/scenarios/usa-normal/cases/d1-03-s04b-36 \
  data/scenarios/loading-next-slow --config /tmp/mgs-slow.toml --trace-loading --period-us 1000
python3 scripts/mgs_scenarios.py run data/scenarios/usa-normal/cases/d1-03-s04b-36 \
  data/scenarios/loading-next-fast --config /tmp/mgs-fast.toml --trace-loading --period-us 1000
python3 scripts/mgs_loading_check.py \
  data/scenarios/loading-next-slow/session data/scenarios/loading-next-fast/session \
  --disc 'data/discs/usa/disc1/Metal Gear Solid (USA) (Disc 1) (v1.0).bin' \
  --output data/scenarios/loading-next-comparison.json
```

L'audit rifiuta sessioni incomplete, configurazioni modificate dopo la prova,
percorsi/carte differenti, settori accelerati fuori dagli archivi ammessi,
sequenze di caricamento differenti e peggioramenti temporali. Confronta
anche scena corrente, difficoltà, disco, vita e inventario agli endpoint.
