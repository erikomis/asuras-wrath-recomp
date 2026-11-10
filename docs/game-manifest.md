# Asura's Wrath (PS3) — manifesto do jogo

Inventário do dump, a primeira fase do trabalho (como no port do Afro
Samurai).

## Identificação

| Campo | Valor |
|---|---|
| Title ID | BLUS30721 (EUA, En/Ja/Fr/De/Es/It) |
| Versão (APP_VER / VERSION) | 01.02 / 01.00 do PARAM.SFO do disco |
| Firmware mínimo (PS3_SYSTEM_VER) | 03.73 |
| Imagem | `Asura's Wrath (USA) (En,Ja,Fr,De,Es,It).iso`, 6,7 GB |
| Atualização do jogo | **o disco já é 01.02** — o EBOOT do patch 1.02 oficial é byte-idêntico ao do disco (sha256 conferido); as DLCs exigem 1.02 |
| Troféus | `TROPDIR/NPWR02376_00` no disco; +`TROPHY_C04_SF.XXX` na Part IV |

## Executável

| Item | Valor |
|---|---|
| `PS3_GAME/USRDIR/EBOOT.BIN` | SELF v2, key_rev **0x0016**, tipo APP (disco), 10.629.688 bytes |
| `game/EBOOT.elf` (descriptografado) | 27.141.464 bytes, sha256 `065c1f0c5f6203c4007730dbbd53edcf9f80d242ee78ab91d8e69198b723a342` |
| Entry OPD / TOC | 0x01994138 → código 0x00E05D5C, TOC **0x019AF8F8** |
| Funções (OPD) | **30.350** (39.507 entradas OPD); 2,4× o Afro Samurai |
| EBOOT do patch 1.02 | NPDRM key_rev 0x0010, licença 3 (free); descriptografado = idêntico ao do disco |
| PRX próprios | nenhum (só bibliotecas de firmware via HLE) |

## Motor e middleware

- **Unreal Engine 3** — o build "BCGAME" da CyberConnect2: `USRDIR/BCGAME/`
  com `COOKEDPS3/` (2.230 arquivos `.XXX` = upk cookado, 4,4 GB),
  `MOVIES/` (69 arquivos, 2,0 GB), `PS3TOC*.TXT` (o TOC do UE3 por idioma:
  INT/JPN/FRA/DEU/ESN/ITA).
- Vídeos Bink (o jogo é quase um anime interativo: `MOVIES/` dominando o
  disco, e shaders Bink PS3 em `BINARIES/PS3/BINK/`).
- Áudio: Unreal Engine 3 padrão no PS3 não é FMOD; conferir pelos imports e
  pelos ELFs SPU quando o SPU for extraído.
- `PS3-BCGAME.INI.EDAT` aparece na Part IV (config de DLC cifrada).

## Imports de firmware (292 em 21 bibliotecas)

cellSpurs 39, cellSysutil 35, sysPrxForUser 33, cellGcmSys 28, sys_net 21,
sys_fs 19, sceNp 15, sys_io 14, **sceNpCommerce2 14** (loja/DLC),
sceNpTrophy 13, **cellResc 11** (o RSX scaler — o jogo renderiza baixo e
escala), cellRtc 8, cellNetCtl 7, cellAudio 7, cellPngDec 7, **cellGame 6**
(detecção de conteúdo adicional), cellHttp 6, cellSysmodule 3, cellSsl 3,
cellOvis 2, cellUserInfo 1.

Sem cellSaveData na lista — conferir como o save é feito (o Afro também não
importava e salvava por arquivo). `cellResc` é novo para os ports do autor:
verificar o suporte no toolkit.

## DLCs

Ver [DLC.md](DLC.md). Resumo: Part IV: Nirvana (final verdadeiro, episódios
19-22), Lost Episodes 1-2 (Street Fighter), interlúdios 11.5 e 15.5, patch
1.02. Todas desempacotadas em `dlc/extracted/` e linkadas no VFS
(`/dev_hdd0/game/BLUS30721/USRDIR/DLC/...`).

## Estrutura do disco

```
PS3_GAME/USRDIR/
  EBOOT.BIN              10,6 MB  (SELF key_rev 0x16)
  BCGAME/                6,4 GB   o jogo UE3
    COOKEDPS3/           4,4 GB   2.230 .XXX (mapas S0xx-S18x, menus, etc.)
    MOVIES/              2,0 GB   69 vídeos (bik)
    PS3TOC*.TXT                   TOC do UE3 por idioma
  BINARIES/PS3/                   ferramentas de build (PS3TOOLS.DLL etc.,
                                  restos do pipeline da CC2 — não roda no PS3)
PS3_UPDATE/PS3UPDAT.PUP          firmware (não é patch do jogo)
```

Os `COOKEDPS3` seguem o padrão `BC-<NOME>_S<mapa>_SF.XXX` — os episódios
19-22 do final verdadeiro (S19x-S22x) **não estão no disco**: só na DLC
`DLCBIGEXPAND0000`.
