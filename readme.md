# 1. Segmentagao e Tamanho do Buffer:
## a. Como o arquivo sera dividido? Qual o tamanho méaximo de dados por datagrama UDP (payload)?
```
  - Conforme o RFC 8200 o IPv6 requer que todo link tenha pelo menos 1280 bytes ou que o link realize
    a fragmentação e remontagem em uma camada abaixo do IPv6. No RFC 721, o IPv4 exige que
    todo modulo da internet deve ser capaz de passar um datagrama de 68 bytes sem fragmentação.
    E conforme o RFC 894: "The minimum length of the data field of a packet sent over an
   Ethernet is 1500 octets, thus the maximum length of an IP datagram
   sent over an Ethernet is 1500 octets"

  - Dado o limite de 1500 bytes do IPv4, os 60 bytes de header IP (RFC 791), os 8 bytes do header
    UDP (RFC 768) e os 12 bytes do header da aplicação. Decidi dividir o arquivo em blocos de 1420 bytes.
```
c. Os buffers de envio (servidor) e recepgao (cliente) precisam ter tamanhos relacionados?
```
  - Os buffers precisam estar relacionados para evitar overflow e, então, perda de pacotes.
```
2. Detecção de Erros:
a. Como a integridade dos dados em cada segmento sera verificada?
```
  - O UDP possui o mesmo mecanismo de checksum do TCP, então não se faria necessário um checksum em
  camada de aplicação para verificar a integridade de pacotes entregues, porém, diferente do IPv6,
  no IPv4 o checksum é opcional. Portanto se faz necessário um checksum em camada de aplicação.
  
```
b. É necessário implementar um checksum? Qual algoritmo usar? (Ex: CRC32, soma simples).
```
  - Eu utilizei um CRC8.
```
  
3. Ordenagao e Detecção de Perda:
a. Como o cliente sabera a ordem correta dos segmentos? É necessário um número de sequéncia?
```
  - Sim.
```
  
b. Como o cliente detectará que um segmento foi perdido (e não apenas atrasado)?
```
  - O cliente sabe qual sequência de segmento ele está esperando, se o segmento recebido for de sequência
  maior que a esperada, um pacote atrasou ou foi perdido. Ainda, o servidor possui um timer para, caso não
  receba resposta no tempo estimado, reseta o janelamento e recomeça a transmição a partir do ultimo pacote
  que sabe-se que foi entregue.

```
4. Controle de Fluxo/Janela (Opcional Avançado):
a. Considerar como evitar que o servidor envie dados mais rapido do que o cliente consegue processar (embora um controle de fluxo completo seja complexo e possa estar fora do escopo inicial).

```
  - O controle de fluxo é bem simples, quando eu limito hard-coded a quantidade de pacotes o servidor
  pode enviar por pipeline sem receber resposta, a perfomance da aplicação melhora significativamente, assim
  como quando aumento o tamanho do buffer no cliente.

```
5. Mensagens de Controle:
a. Definir claramente os formatos das mensagens de: requisigao de arquivo, envio de segmento de dados (incluindo cabecalhos com
numero de sequéncia, checksum, etc.), confirmacao de recebimento (se houver), solicitagdo de retransmissao e mensagens de erro.
```
  - A aplicação possui flags para cada tipo de pacote, possui um fluxo estabelecido para quando
  enviar requisições e quando há erro, o cliente é notificado e encerrado a aplicação.

```
Itens Obrigatórios a Demonstrar e Explicar:

1. Ambiente e Setup:

a Mostrar a execução do Servídor UDP.

b. Mostrar a execução do(s) Cliente(s) UDP.

<. Demonstrar como o clente especifica/obtém o endereço 1P e a porta do servidor.

d. Mostrar que a implementação utiliza a API de Sockets dietamente, sem biblotecas de

abstração UDP.
2. Protocolo de Aplicação e Requisição:

a Demonsrar o chente requisitando arquivos ao servidor usando o protocolo de aplicação

proposto (Ex: GET /nome arquivo).

b. Mostrar o cliente requisitando com sucesso arquivos diferentes (pelo menos dois).

3. Transferência e Segmentação:

a Demonstrar a transferência de um arquivo grande (ex: > 10 MB).

b. Explicar e demonstrar como o arquivo é divídido em segmentos (chunks) para transmissão

viaUDP.

1. Informar qual o tamanho do buffer/segmento de dados utilizado.
Explicar brevemente a influência do tamanho do segmento e sua relação com o MTU
(Maximum Transmission Unit).
4. Mecanismos de Confiabilidade (Demonstrar e Explicar):
1 Ordenação: Mostrar como os números de sequência são usados para ordenar os
segmentos no chente e detectar perdas.
b Integridade:
1. Demonstrar o uso de checksum ou hash (MDS/SHA) para veríficar a integridade dos
segmentos recebidos. Indicar qual método foi usado.
Mostrar o processo de montagem e conferência do arquivo completo no cliente após o
recebimento.
«. Recuperação de Erros/Perdas:

1. Simular a perda de pacotes: Demonstrar um mecanismo (idealmente no cliente ou
Simulado) que cause a perda de segmentos durante a transmissão. (Nota: O requisito
'original mencionava o servidor descantando, o que é incomum. É mais didático simular a
perda na rede ou no cliente para testar a recuperação).

Mostrar o clente detectando a faka de segmentos (por timecut, NACKs, ou análise de
sequência).
11 Demonstrar o chente sotcitando a retansmissão dos segmentos fatantes/corrompedos
00 servidor.
1V Explicar o mecanismo usado para disparo da retransmissão (Ex: timer de timeout,
ACKS/NACKs).
S. Tratamento de Erros do Servídor:

a Demonstrar o cenário em que o clente requista um arquivo inexistente,

b Mostrar o senvídor enviando uma mensagem de erro definida no protocolo.

. Mostrar o cliente recebendo e apresentando o erro de forma clara (Ex: “Erro: Arquivo não

encontrado”).

« Mencionar/Demonstrar outros erros tratados. se houver.

6 Cenários de Teste Específicos:

a Demonstrar o serndor hdando com dors clentes simutâneos requistando arquvos

arerentes.

b Mostrar o que acontece se o clente tentar conectar antes do senvídor ser iniciado

(demonstrar o erro resultante).
c.Mostrar o que acontece se o servídor for interrompido durante uma transferência
(demonstrar o comportamento do cliente - ex: timeout, erro).
