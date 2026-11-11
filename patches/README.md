# Patches locais do toolkit

Aplicados nesta sessão em `../ps3recomp-asura` (base observada `6c4ec53a`).
Os arquivos abaixo só contêm alterações do runtime, sem dados do jogo.

- `ps3recomp-gcm-flip-link.patch`: substitui a referência inexistente
  `gcm_flip_request_ext` pela função interna `gcm_flip_request`, com declaração
  antecipada. Corrige a falha de link ao recompilar o toolkit atual.
- `ps3recomp-spurs-wait-signal.patch`: uma espera SPURS sem objeto de fila
  (`wobj == 0`) não pode terminar devido a uma geração de LFQueue de outro
  subsistema. Mantém a espera por sinal da própria tarefa.
- `ps3recomp-trophy-endian.patch`: contagens, IDs e timestamps de troféus
  em big endian, com os tamanhos corretos das estruturas públicas. Evita
  interpretar 32 troféus como 536.870.912.
- `ps3recomp-user-allocation-capacity.patch`: comporta todas as páginas de
  64 KB do orçamento de memória e procura intervalos livres em ordem, sem
  esgotar uma tabela de 1.024 entradas antes de esgotar os bytes disponíveis.
- `ps3recomp-asura-gameplay.patch` (11/11/2026, aplicar depois dos quatro
  acima): o que levou o título até a gameplay do Episódio 1.
  `SPU_CRI_IMAGE` tira do id literal 22 os casos especiais da task CRI de
  outro título (aqui a imagem 22 é a task de PhysX); o lifter deixa de cortar
  em 256 entradas uma tabela de saltos embutida no texto quando não acha o
  limite (`func_00CD65CC`, 373 casos); `PAD_NO_CONTROLLER` mantém o controle
  físico fora de probes com roteiro; `GCM_USER_CMD_QUEUE` entrega um user
  command por chamada do handler; e os diagnósticos `GCM_USER_WATCH` e
  `SPU_PUT_VALWATCH`. Tudo opt-in por variável de ambiente, exceto a correção
  do lifter e `cellSpursQueueDetachLv2EventQueue`, que deixa de ser um import
  não resolvido (o título o chama 40 vezes ao carregar o primeiro mapa).

Em um checkout compatível, confira e aplique cada patch com
`git -C ../ps3recomp-asura apply --check <caminho-absoluto-do-patch>` e
`git -C ../ps3recomp-asura apply <caminho-absoluto-do-patch>`.
Se já estiver aplicado, não reaplique.

Validação de WAIT_SIGNAL:

```sh
python3 tools/test_spurs_plain_signal.py --toolkit ../ps3recomp-asura
```

O teste adapta o harness de sinalização do SDK para atravessar a syscall
WAIT_SIGNAL com atividade anterior em uma fila não relacionada: 11 verificações
passaram, incluindo sinais para outra tarefa e outro taskset que não devem
liberar a espera.

Outras regressões do toolkit:

```sh
python3 ../ps3recomp-asura/tools/test_trophy_registration.py
python3 ../ps3recomp-asura/tools/test_user_memory_domain.py
```

Passaram, respectivamente, o teste de troféus e 3.446 verificações do
alocador. O teste `test_loaded_image_memory.py` também passou 26 verificações
de preservação dos intervalos de imagens ELF carregadas.
