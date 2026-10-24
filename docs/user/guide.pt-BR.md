<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Jadyx: da instalação ao primeiro jogo

[English](guide.md) · [Español](guide.es.md)

Siga as três partes em ordem. Você precisa de um Xbox Series X ou S já ativado no **Modo de Desenvolvedor**, um controle e um computador na mesma rede local. Device Portal é o site de gerenciamento do Xbox; Portal Jadyx é outro site, iniciado dentro do Jadyx para importar arquivos.

Prepare somente dumps do **seu próprio Switch e de jogos que você possui**: `prod.keys`, `title.keys` quando necessário, firmware e jogos. Jadyx não inclui nenhum deles. Não peça nem compartilhe esses arquivos nos canais de suporte.

## Parte 1 · Instale o Jadyx

1. Baixe `Jadyx_<version>_x64.appx` e `Microsoft.VCLibs.x64.14.00.appx` na [página de versões do Jadyx](https://github.com/JulianDr14/jadyx/releases/latest). Guarde os dois no computador.
2. No Xbox, abra **Dev Home**. Em **Remote Access**, ative **Xbox Device Portal** e configure usuário/senha se solicitado. Anote o endereço mostrado, normalmente `https://192.168.x.x:11443`.
3. Abra esse endereço no navegador do computador. O aviso de certificado é normal nessa página local: prossiga somente depois de conferir que o endereço é do seu Xbox. Entre com as credenciais de Remote Access. A página do Device Portal deve abrir.
4. Na página **Home**, escolha **Add**, selecione o `.appx` do Jadyx e depois **Next**. Quando pedir dependências, adicione `Microsoft.VCLibs.x64.14.00.appx` e escolha **Start**. Espere terminar: Jadyx deve aparecer no Dev Home. O `.cer` **não é necessário no Modo de Desenvolvedor do Xbox**; o mantenedor confirmou a instalação com APPX + VCLibs no Series X em 23–24 de outubro de 2026.
5. No Dev Home do Xbox, selecione Jadyx, abra **View details** e mude **App type** para **Game**. Confira antes de abrir. Como App, há cerca de **1 GB** de memória; como Game, Jadyx recebe cerca de **5 GB (5120 MiB)**. Isso é RAM, não espaço para arquivos.

Para atualizar depois, instale o novo APPX sobre o existente. **Não desinstale antes**: isso apaga chaves, firmware, saves e jogos internos do aplicativo.

> **Em breve:** uma instalação mais simples. Por enquanto, siga os passos de APPX + VCLibs acima.

## Parte 2 · Coloque suas chaves, firmware e jogos

O armazenamento interno do Modo de Desenvolvedor pode ter só alguns GB livres, mesmo com um SSD grande no Xbox. Deixe jogos grandes em um drive USB; envios pelo portal usam armazenamento interno.

### Chaves e firmware: Portal Jadyx

1. Abra Jadyx. Use os botões superiores do controle (LB/RB) para chegar a **Configurações** e abra **Gerenciador de arquivos**. Selecione **Portal Jadyx**, cuja ação é **Iniciar portal**. Aparece uma tela com endereço local e **Código de conexão**.
2. No computador, abra o endereço mostrado pelo Jadyx, não o do Device Portal. Digite o código. Os dois aparelhos devem estar na mesma rede local. Se expirar, escolha **Gerar código de conexão** no Jadyx e use o novo código.
3. No navegador, abra **Sistema > Chaves**. Em **Selecionar arquivos**, escolha seu `prod.keys` e também `title.keys` se o dump precisar, ou um ZIP com eles. Clique em **Enviar e revisar**. Confira versões e avisos, depois **Confirmar instalação**. Não aceite um aviso sem conferir os arquivos de origem.
4. Em **Sistema > Firmware**, escolha seu ZIP de firmware ou os arquivos `.nca` extraídos (o navegador também oferece **Ou escolha uma pasta extraída**). Clique em **Enviar e revisar**, confira o resultado e depois **Confirmar instalação**. Importe as chaves primeiro: elas precisam conseguir ler esse firmware. Espere **Instalação concluída** antes de sair.
5. De volta ao Gerenciador de arquivos do Jadyx, confira **Chaves prontas** e **Firmware: … arquivos**. A contagem confirma a presença dos arquivos; não garante que todos os jogos funcionem.

As chaves são importadas para `LocalState\jadyx\keys` e carregadas pelo app. Para copiar suas próprias chaves manualmente pelo Device Portal, feche Jadyx, abra **File explorer > LocalAppData > pacote do Jadyx > LocalState > jadyx > keys**, envie `prod.keys` e, se necessário, `title.keys`, e reabra Jadyx. Uma pasta `keys` qualquer na raiz do USB não é esse local.

### Jogos: USB para arquivos grandes, ou armazenamento interno

- **USB (recomendado para jogos grandes):** copie seus dumps completos `.nsp` ou `.xci` para uma pasta no drive; homebrew `.nro` também é aceito. Use um sistema de arquivos que permita arquivos acima de 4 GiB, como NTFS; FAT32 não permite. Conecte ao Xbox como armazenamento de mídia. **Não escolha “games & apps” nem aceite formatar**, pois isso apaga o drive. Em **Configurações > Gerenciador de arquivos > Adicionar uma pasta de jogos**, escolha essa pasta no seletor. Os jogos ficam no USB; mantenha conectado ao jogar. Subpastas também são lidas.
- **Envio pelo navegador:** no Portal Jadyx, abra **Conteúdo > Jogos > Selecionar arquivos NSP**, escolha um `.nsp` de jogo base e clique em **Enviar / retomar**. Espere a validação e a inclusão na biblioteca. Esse caminho copia para o armazenamento interno e aceita jogos base NSP, não XCI ou atualizações/DLC.
- **Alternativa pelo Device Portal:** com Jadyx fechado, envie os dumps por **File explorer > LocalAppData > pacote do Jadyx > LocalState > games**. Reabra Jadyx depois. É `LocalState\games`, separado de `LocalState\jadyx`.

Para suas atualizações/DLC, adicione primeiro o jogo base. Na biblioteca, aperte **Menu** nele, escolha **Atualizações** ou **DLC** e depois **Adicionar conteúdo…**. Escolha **USB / pasta original · sem cópia** ou **Armazenamento interno**, selecione um pacote NSP/XCI completo, revise e confirme. Mantenha conectado o conteúdo externo. Só uma atualização fica ativa; DLC podem ser ativados individualmente. Não envie atualizações como jogos base.

## Parte 3 · Jogue

1. Volte à **Biblioteca**. Se um jogo recém-copiado não aparecer, aperte **X** para verificar novamente. Selecione o jogo e aperte **A** para jogar. Deixe o USB conectado se o jogo ou conteúdo selecionado estiver nele.
2. Espere enquanto os shaders compilam. A primeira visita a uma área pode ter travadas; compatibilidade e desempenho variam por jogo. Jadyx é beta.
3. Segure **View + Menu** para abrir o menu com o jogo pausado. Escolha **Continuar** para jogar ou **Voltar ao início** para retornar à biblioteca; espere o retorno terminar antes de fechar o app.

**Series S:** use o mesmo instalador e a opção Game. Há relatos da comunidade no Series S no [README](../../README.md#progress), mas resultados do Series X não garantem o mesmo desempenho ou compatibilidade no S. Informe o modelo nos relatos.

### Se algo der errado

- **Pedido de dependência / falta Microsoft.VCLibs:** volte à Parte 1 e forneça o APPX x64 de VCLibs como dependência, não como pacote do Jadyx.
- **“The existing keys cannot read this content” (as chaves existentes não conseguem ler o conteúdo):** confira/importe seu próprio `prod.keys` compatível. Se um dump de atualização for protegido por direitos e não tiver ticket, a chave de título correspondente precisa estar no seu próprio `title.keys`; `prod.keys` sozinho não a fornece. Reimporte ambos em Chaves e tente novamente. Confira também se o dump está completo. Nunca publique as chaves.
- **Firmware ilegível/incompleto:** importe chaves compatíveis primeiro, depois um dump completo de firmware do seu console. Uma pasta com NCA de jogos não é firmware.
- **Envio falha / falta de espaço:** confira o espaço interno livre do Modo de Desenvolvedor pelo Device Portal. Envios podem falhar quando esse disco está cheio. Libere arquivos de que não precisa mais, preserve saves e use USB para jogos grandes. A capacidade anunciada do SSD do Xbox não é o espaço disponível para o app.
- **`bad allocation` / app fecha:** confira primeiro **App type: Game**. Mesmo como Game, um jogo exigente pode ultrapassar o limite de aproximadamente 5 GB.
- **Jogo USB ausente/ilegível:** confira a conexão e selecione a pasta novamente em **Adicionar uma pasta de jogos**. Confira se a cópia terminou; só o caminho não dá permissão ao app.
- **Tela preta ou crash após essas verificações:** reúna os dois logs abaixo; tela preta sozinha não identifica a causa.

### Pegue os logs

Abra **Device Portal > File explorer > LocalAppData** do Xbox, selecione o pacote do Jadyx e depois **LocalState > jadyx > log**. Baixe **os dois**:

- `LocalState\jadyx\log\jadyx_diag.txt`
- `LocalState\jadyx\log\jadyx_log.txt`

Envie ao [suporte do Jadyx](https://discord.gg/3BhyyXGxpd), com versão do app, modelo Series X/S, se está como Game, o que fez e o que aconteceu. Nunca inclua chaves, firmware ou jogos.
