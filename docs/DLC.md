# Asura's Wrath — DLC no port (foco: o final verdadeiro)

🇬🇧 Português (este arquivo) · [English](DLC.en.md) *(a traduzir)*

O jogo vendeu o final da história como conteúdo adicional. Este documento
descreve as DLCs, como o port as obtém, e o que falta para cada uma rodar no
macOS. **A prioridade é a Part IV: Nirvana (episódios 19-22), o "true
ending"** — sem ela, o jogo termina no Episódio 18 com um cliffhanger.

## As DLCs

| Content ID | O que é | Tamanho | Estado no port |
| --- | --- | --- | --- |
| `DLCBIGEXPAND0000` | **Episode Pack Part IV: Nirvana** — episódios 19-22, o final verdadeiro | 1,9 GB | baixada, extraída, linkada no VFS |
| `DLCCOLLABO010000` | Lost Episode 1: *At Last, Someone Angrier Than Me* (vs Ryu, Street Fighter) | 174 MB | baixada, extraída, linkada no VFS |
| `DLCCOLLABO020000` | Lost Episode 2: *The Strongest vs. the Angriest* (vs Akuma) | 268 MB | baixada, extraída, linkada no VFS |
| `ASIAEPISODE_11_5` | Episode 11.5: *Forging Ahead* (interlúdio, estilo anime) | 1,1 GB | não baixada (ver abaixo) |
| `ASIAEPISODE15PS3` | Episode 15.5: *Defiance* (interlúdio, estilo anime) | 1,3 GB | não baixada (ver abaixo) |
| `PATCH02000000000` | Patch 1.02 do jogo | 35 MB | baixado; **o EBOOT dele é byte-idêntico ao do disco** (o disco já é 01.02), então só serve de conferência |

Os interlúdios 11.5 e 15.5 ficam de fora do download padrão só por espaço em
disco: `tools/fetch_dlc.sh FETCH_EXTRA=1` traz os dois (as URLs estão no
script).

## Como obter e instalar

Nada de conteúdo de jogo no repositório: cada um baixa da CDN da Sony a
própria cópia, com a própria conta/região (BLUS30721, USA).

```sh
tools/fetch_dlc.sh                  # baixa Part IV + Lost Episodes + patch
tools/extract_dlc.sh                # desempacota e linka no VFS
tools/rebuild.sh dlc                # os dois de cima, pelo pipeline padrão
```

O que cada etapa faz:

1. **fetch**: `curl` dos pacotes `.pkg` oficiais para `dlc/pkg/`. As URLs são
   as públicas da CDN da Sony para o BLUS30721; o content id de cada arquivo
   é conferido no cabeçalho do próprio pacote, não em listas de terceiros
   (elas trocam os episódios entre si — aconteceu).
2. **extract**: `tools/pkg_extract.py` (do toolkit) descriptografa o pacote —
   keystream retail GPKG — e escreve a árvore original dele em
   `dlc/extracted/<content id>/`. Depois da extração boa, o `.pkg` pode ser
   apagado (`extract_dlc.sh` não apaga; `KEEP_PKGS=0` não implementado).
3. **link**: `tools/link_dlc.sh` liga a árvore por symlink folha em
   `rpcs3/dev_hdd0/game/BLUS30721/` — exatamente o layout que o instalador
   da Sony escreve no console, que é onde o jogo procura as DLCs
   (`/dev_hdd0/game/BLUS30721/USRDIR/DLC/`). `tools/run.sh` já sobe esse
   diretório como `/dev_hdd0` do jogo.

## O que tem dentro da Part IV (o final verdadeiro)

Extraída em `dlc/extracted/DLCBIGEXPAND0000/USRDIR/DLC/DLCBIGEXPAND/`:
617 arquivos de Unreal Engine 3 cooked:

- `TEXTURES_DLCBIGEXPAND.TFC` (280 MB) e `LIGHTING_DLCBIGEXPAND.TFC` — os
  caches de textura e iluminação dos episódios 19-22;
- `S190_SF.XXX` … `S22*_SF.XXX` — os mapas dos episódios 19 a 22
  (números de mapa `S19x`, `S20x`, `S21x`, `S22x`);
- `INTERVALDRAMA_S210/S220/S230/S250_SF.XXX` — os interlúdios entre os
  episódios (o drama entre capítulos);
- `TOBECONTINUED_S2xx_SF.XXX` — as telas "to be continued" dos episódios;
- `TROPHY_C04_SF.XXX` — os troféus da parte IV;
- `PS3-BCGAME.INI.EDAT` — a configuração do DLC, **criptografada** (ver
  bloqueio abaixo);
- `INTERVALDRAMA.{INT,JPN,FRA,DEU,ESN,ITA}` — textos dos interlúdios em cada
  idioma (o disco é multi-idioma: EnJaFrDeEsIt).

## Bloqueios conhecidos (DLC)

- **`PS3-BCGAME.INI.EDAT`**: o jogo precisa ler este INI para descobrir as
  DLCs instaladas, e ele vem cifrado no EDAT da Sony (licença livre, tipo 3).
  No console é o firmware que decifra; o port precisa fazer o mesmo — a
  decisão é: decifrar no HLE do `cellFs` (como o RPCS3 faz, transparente ao
  jogo) ou pré-decifrar em build e redirecionar a leitura. Provavelmente a
  primeira, porque há um `.EDAT` por DLC.
- **Executável**: o disco já é 1.02 (o EBOOT do patch 1.02 é byte-idêntico ao
  do disco — conferido por SHA-256), então não há EBOOT de DLC para
  recompilar: as DLCs do Asura's Wrath são só conteúdo (mapas/texturas/filmes
  UE3), o executável é um só. Isso simplifica muito o port.
- **Troféus**: `TROPHY*.TRP`/`TROPHY_C04_SF.XXX` ficam fora de escopo por
  enquanto (como nos outros ports).

## Referências

- SerialStation: content ids do BLUS30721 e das DLCs.
- Shacknews (27/03/2012): anúncio de preço/data da Part IV ($6.99, 24/04/2012).
- Asura's Wrath Wiki (Fandom): Part IV: Nirvana, episódios 19-22.
