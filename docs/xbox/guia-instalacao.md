# Instalar e preparar o Jadyx no Xbox

Você precisa de um Xbox Series X|S já no Modo de Desenvolvedor e das chaves, do firmware e dos
jogos extraídos do seu próprio Switch. O Jadyx não fornece esses arquivos.
O Device Portal do Xbox instala o aplicativo. O Jadyx Portal recebe seus arquivos depois.
São páginas diferentes. Os nomes em inglês abaixo são os botões do Xbox e do portal.

1. Baixe o `.appx` do Jadyx nos arquivos da versão.
2. Baixe `Microsoft.VCLibs.x64.14.00.appx` da mesma versão.
3. Abra **Dev Home** no Xbox.
4. Anote o IP mostrado em **Remote Access**.
5. Abra `https://<IP-do-Xbox>:11443` no navegador de um aparelho na mesma rede.
6. Abra **Apps > Add** no Device Portal.
7. Selecione o `.appx` do Jadyx como aplicativo.
8. Selecione `Microsoft.VCLibs.x64.14.00.appx` como dependência.
9. Inicie a instalação e espere terminar.
10. Selecione Jadyx em **Dev Home** no Xbox.
11. Aperte **View** no controle para abrir **View details**.
12. Mude **App type** para **Game** (também em **Apps > Jadyx > App type** no Device Portal).
13. Abra o Jadyx novamente após mudar o modo.
14. Abra **Ajustes > Arquivos > Conexão de rede > Iniciar servidor** no Jadyx.
15. Abra o endereço mostrado pelo Jadyx no navegador de um aparelho na mesma rede.
16. Digite o código mostrado pelo Jadyx.
17. Importe seu `prod.keys` em **Sistema > Chaves** no Jadyx Portal.
18. Importe seu ZIP de firmware ou os arquivos NCA extraídos em **Sistema > Firmware**.
19. Confirme as versões mostradas na revisão da importação.
20. Envie um NSP de jogo base em **Jogos**.
21. Envie atualizações em **Biblioteca > seu jogo > Atualizações**.
22. Envie DLC em **Biblioteca > seu jogo > DLC**.

Instale também o `Jadyx.cer` como certificado no Device Portal. Estamos testando como tirar esse passo
para simplificar a instalação, mas por enquanto ele continua.

O modo Game oferece 5120 MiB. O modo App tem pouca memória para o emulador. Um aviso permanente
na biblioteca explica a mudança quando o Xbox informa um limite menor. A lista de preparação
aparece quando faltam chaves utilizáveis ou arquivos de firmware; quem já configurou mantém a
biblioteca normal.

`title.keys` contém chaves por título para suas cópias criptografadas com tickets; não substitui
`prod.keys`. Se as chaves não leem um firmware ou uma atualização mais recente, extraia chaves
compatíveis do seu console com esse firmware ou mais recente. O índice da geração das chaves não
é a versão do firmware. Quando a versão não pode ser descriptografada, o Jadyx não consegue nomeá-la.

## Pelo USB

1. Coloque as chaves em uma pasta do USB.
2. Coloque o ZIP do firmware ou os NCAs extraídos em outra pasta.
3. Selecione a pasta correspondente em **Ajustes > Arquivos**.
4. Adicione a pasta dos jogos externos nessa mesma tela.

O seletor escolhe pastas, inclusive quando há um ZIP dentro. Não selecione uma pasta que misture
firmware e jogos. O app importa firmware e chaves para o armazenamento interno.

## Cópia manual pelo Device Portal

1. Feche o Jadyx.
2. Abra **File explorer > LocalAppData > Jadyx > LocalState** no Device Portal.
3. Copie `prod.keys` e, se necessário, `title.keys` para `jadyx/keys`.
4. Copie os NCAs de firmware já extraídos para `jadyx/nand/system/Contents/registered`.
5. Copie os jogos base para `games`.
6. Abra o Jadyx novamente.

Selecione o pacote instalado do Jadyx se o portal mostrar o nome da família do pacote.
A cópia manual não faz a revisão de importação; prefira o importador do app ou o Jadyx Portal.

## Se parecer travado

Veja os bytes recebidos e confirmados. **Validando** verifica o pacote; **Adicionando** publica o
jogo. Após 60 segundos sem progresso na importação do firmware, o aviso mostra o arquivo atual.
Verifique a conexão USB e o espaço livre antes de tentar novamente. Quando falta armazenamento,
o erro mostra GB decimais livres e necessários, incluindo espaço temporário. Ao extrair ZIP, a
estimativa cobre a entrada atual, não o arquivo inteiro antecipadamente.

Se enviar uma atualização ou DLC em Jogos, o aviso indica a página correta da biblioteca.
Apague a transferência pendente antes de enviá-la de novo na página indicada.

Para atualizar o Jadyx, instale o pacote novo sobre o antigo. **Não desinstale primeiro**: isso
apaga chaves, firmware, saves e jogos internos. Os diagnósticos ficam em `LocalState/jadyx/log`.
