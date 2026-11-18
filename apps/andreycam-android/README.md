# AndreyCam Mobile

Cliente Android do AndreyCam. Abra esta pasta no Android Studio e gere o APK em **Build → Build APK(s)**. O projeto usa apenas APIs do Android, sem bibliotecas de terceiros; exige JDK 17/Android Studio e Android 8.0 ou posterior. A primeira compilação baixa o Android Gradle Plugin e a plataforma SDK 35.

## Usar

1. No Andrey OS, abra **AndreyCam** e mantenha o computador e o telefone na mesma rede local.
2. No telefone, informe o endereço IPv4, a porta `47017` e o código temporário de seis dígitos mostrados no desktop.
3. Toque em **Conectar**. Quando iniciar a transmissão no desktop, permita o sensor solicitado pelo modo escolhido.
4. Escolha os dispositivos de entrada no desktop e toque em **Iniciar conexão**.

O app Android mantém a tela ativa durante a transmissão. Ele não envia áudio nem vídeo antes de receber o comando de início do desktop e só pede a permissão do sensor escolhido.

## Protocolo local v1

O desktop aceita uma conexão TCP na porta 47017. O cliente começa com `ANDREYCAM/1 <código>\n`; o servidor responde `OK\n` ou fecha a conexão. O servidor envia `START <modo>\n` (`0` microfone, `1` câmera, `2` ambos) e `STOP\n`. Depois do início, o telefone envia registros binários: um byte `V` seguido de tamanho uint32 big-endian e JPEG, ou byte `A` seguido de tamanho uint32 big-endian e PCM signed 16-bit little-endian, mono, 48 kHz. O código é aleatório a cada execução, mas o tráfego não usa TLS; conecte apenas em uma rede local confiável.
