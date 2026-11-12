# Andrey OS
Sistema operacional focado em privacidade, desempenho e velocidade.Um desktop limpo, leve e sob controle. Sem telemetria, sem serviços desnecessários e sem atrito.O que prioriza
Privacidade

Nenhum dado é coletado. O sistema não tenta “melhorar a experiência” às custas do usuário.
Desempenho

Ambiente enxuto. Recursos ficam disponíveis para o que realmente importa.
Velocidade

Interface direta, resposta rápida e atualizações que não forçam reinstalação.
FilosofiaMenos camadas entre o usuário e a máquina.

Menos ruído.

Mais controle.


## Drivers e hardware

O Andrey OS já vem com firmware (`firmware-linux`, `firmware-misc-nonfree`, SOF), microcódigo Intel/AMD,
Mesa (OpenGL/Vulkan), aceleração de vídeo (VA-API) e fwupd. O que depende do seu hardware é decidido pelo
**Drivers e hardware** (`andrey-drivers`, também no menu):

- `andrey-drivers` — janela: detecta CPU/GPU/Wi-Fi/VM e instala o que falta
- `andrey-drivers --report` — relatório em texto (não instala nada)
- `andrey-drivers --auto` — instala tudo que for recomendado, sem janela
- `andrey-drivers --newer` — kernel, Mesa, firmware e NVIDIA mais novos (trixie-backports)
- `andrey-drivers --firmware` — atualizações de BIOS/SSD/periféricos via fwupd

NVIDIA: o pacote certo (`nvidia-driver` ou um `legacy`) vem do `nvidia-detect`. AMD e Intel usam o driver do
kernel + Mesa (não existe "driver da AMD/Intel para baixar" no Linux: o do Debian já é o oficial).
Com Secure Boot ligado, o assistente cria a chave do DKMS e pede uma senha para cadastrar no MOK (tela azul no
próximo boot). Depois do login, um aviso único oferece instalar o que faltar.
