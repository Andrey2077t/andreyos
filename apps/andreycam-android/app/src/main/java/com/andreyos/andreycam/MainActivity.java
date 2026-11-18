package com.andreyos.andreycam;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.hardware.camera2.*;
import android.media.*;
import android.os.*;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.*;
import java.io.*;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Collections;

public final class MainActivity extends Activity {
    private static final int PERMISSIONS = 1;
    private EditText host, port, code;
    private TextView status;
    private Button connectButton;
    private volatile Socket socket;
    private volatile DataOutputStream output;
    private volatile boolean connected, mediaRunning;
    private volatile int mode = 1;
    private volatile int pendingMode = -1;
    private String cameraId;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader images;
    private HandlerThread cameraThread;
    private Handler cameraHandler;
    private AudioRecord recorder;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL); root.setPadding(dp(24), dp(26), dp(24), dp(20));
        root.setBackgroundColor(Color.rgb(31,5,10));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        TextView title = new TextView(this); title.setText("AndreyCam Mobile"); title.setTextSize(27); title.setTextColor(Color.WHITE); title.setGravity(Gravity.CENTER);
        root.addView(title, new LinearLayout.LayoutParams(-1, dp(60)));
        TextView intro = new TextView(this); intro.setText("Conecte à mesma rede Wi-Fi do computador. Digite o endereço e o código exibidos no AndreyCam do Andrey OS.");
        intro.setTextColor(Color.LTGRAY); intro.setTextSize(15); intro.setPadding(0,dp(12),0,dp(12)); root.addView(intro);
        host = field("IP do computador (ex.: 192.168.1.10)"); root.addView(host);
        port = field("Porta"); port.setText("47017"); root.addView(port);
        code = field("Código de seis dígitos"); root.addView(code);
        TextView note = new TextView(this); note.setText("O computador escolhe se usará câmera, microfone ou ambos. A transmissão só começa quando você iniciar no Andrey OS.");
        note.setTextColor(Color.LTGRAY); note.setPadding(0,dp(12),0,dp(12)); root.addView(note);
        connectButton = new Button(this); connectButton.setText("Conectar"); root.addView(connectButton);
        Button flip = new Button(this); flip.setText("Alternar câmera frontal/traseira"); root.addView(flip);
        status = new TextView(this); status.setText("Aguardando conexão"); status.setTextColor(Color.WHITE); status.setGravity(Gravity.CENTER); status.setTextSize(16);
        root.addView(status, new LinearLayout.LayoutParams(-1,dp(64)));
        TextView keepOpen = new TextView(this); keepOpen.setText("Mantenha este app aberto durante o uso."); keepOpen.setTextColor(Color.GRAY); root.addView(keepOpen);
        setContentView(root); findBackCamera();
        connectButton.setOnClickListener(v -> connectOrDisconnect());
        flip.setOnClickListener(v -> { boolean wasRunning=mediaRunning; stopMedia(); switchCamera(); if (wasRunning) startMedia(mode); else if (connected) showStatus("Câmera alterada"); });
    }

    private EditText field(String hint) {
        EditText e = new EditText(this); e.setSingleLine(true); e.setHint(hint); e.setTextColor(Color.WHITE); e.setHintTextColor(Color.GRAY); return e;
    }
    private int dp(int n) { return (int)(n * getResources().getDisplayMetrics().density + .5f); }

    private void connectOrDisconnect() {
        if (connected) { disconnect(); return; }
        if (host.getText().toString().trim().isEmpty() || code.getText().toString().trim().length()!=6) { showStatus("Informe o IP e o código de seis dígitos."); return; }
        connect();
    }

    @Override public void onRequestPermissionsResult(int request, String[] names, int[] grants) {
        super.onRequestPermissionsResult(request,names,grants);
        if (request==PERMISSIONS && pendingMode>=0 && grants.length>0 && Arrays.stream(grants).allMatch(x->x==PackageManager.PERMISSION_GRANTED)) {
            int selected=pendingMode; pendingMode=-1; startMedia(selected);
        } else if (request==PERMISSIONS) { pendingMode=-1; showStatus("A permissão foi negada; a transmissão não começou."); }
    }

    private void connect() {
        final String ip=host.getText().toString().trim(), pin=code.getText().toString().trim();
        final int tcpPort;
        try { tcpPort=Integer.parseInt(port.getText().toString().trim()); } catch(Exception e) { showStatus("Porta inválida"); return; }
        connectButton.setEnabled(false); showStatus("Conectando…");
        new Thread(() -> {
            try {
                Socket s=new Socket(ip,tcpPort); s.setKeepAlive(true); s.setTcpNoDelay(true);
                DataOutputStream out=new DataOutputStream(s.getOutputStream());
                out.write(("ANDREYCAM/1 "+pin+"\n").getBytes("UTF-8")); out.flush();
                BufferedReader input=new BufferedReader(new InputStreamReader(s.getInputStream(),"UTF-8"));
                if (!"OK".equals(input.readLine())) { s.close(); throw new SecurityException("Código incorreto ou conexão recusada."); }
                socket=s; output=out; connected=true;
                runOnUiThread(() -> { connectButton.setText("Desconectar"); connectButton.setEnabled(true); showStatus("Conectado ao Andrey OS"); });
                String cmd;
                while (connected && (cmd=input.readLine())!=null) {
                    if (cmd.startsWith("START ")) { mode=Integer.parseInt(cmd.substring(6).trim()); requestMediaPermissions(mode); }
                    else if ("STOP".equals(cmd)) { pendingMode=-1; stopMedia(); showStatus("Conectado; transmissão pausada"); }
                }
            } catch(Exception e) { showStatus("Não conectou: "+e.getMessage()); }
            finally { disconnect(); }
        },"AndreyCam-Network").start();
    }

    private void startMedia(int selected) {
        stopMedia(); mediaRunning=true;
        if (selected!=0) openCamera();
        if (selected!=1) openMicrophone();
        showStatus("Transmitindo para o Andrey OS");
    }

    private void requestMediaPermissions(int selected) {
        runOnUiThread(() -> {
            if (!connected) return;
            boolean needCamera=selected!=0 && checkSelfPermission(Manifest.permission.CAMERA)!=PackageManager.PERMISSION_GRANTED;
            boolean needMic=selected!=1 && checkSelfPermission(Manifest.permission.RECORD_AUDIO)!=PackageManager.PERMISSION_GRANTED;
            if (!needCamera && !needMic) { startMedia(selected); return; }
            pendingMode=selected;
            if (needCamera && needMic) requestPermissions(new String[]{Manifest.permission.CAMERA,Manifest.permission.RECORD_AUDIO},PERMISSIONS);
            else requestPermissions(new String[]{needCamera?Manifest.permission.CAMERA:Manifest.permission.RECORD_AUDIO},PERMISSIONS);
        });
    }

    private void openCamera() {
        if (cameraId==null || checkSelfPermission(Manifest.permission.CAMERA)!=PackageManager.PERMISSION_GRANTED) { showStatus("Câmera indisponível ou sem permissão"); return; }
        cameraThread=new HandlerThread("AndreyCam-Camera"); cameraThread.start(); cameraHandler=new Handler(cameraThread.getLooper());
        try {
            CameraManager manager=(CameraManager)getSystemService(Context.CAMERA_SERVICE);
            android.hardware.camera2.params.StreamConfigurationMap map=manager.getCameraCharacteristics(cameraId)
                    .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            android.util.Size chosen=null;
            if(map!=null) for(android.util.Size size:map.getOutputSizes(android.graphics.ImageFormat.JPEG))
                if(size.getWidth()<=1280 && size.getHeight()<=720 && (chosen==null || size.getWidth()*size.getHeight()>chosen.getWidth()*chosen.getHeight())) chosen=size;
            if(chosen==null && map!=null) { android.util.Size[] choices=map.getOutputSizes(android.graphics.ImageFormat.JPEG); if(choices.length>0) chosen=choices[0]; }
            if(chosen==null) throw new IllegalStateException("O celular não oferece captura JPEG.");
            images=ImageReader.newInstance(chosen.getWidth(),chosen.getHeight(),android.graphics.ImageFormat.JPEG,2);
            images.setOnImageAvailableListener(reader -> {
                Image image=reader.acquireLatestImage(); if(image==null)return;
                try { ByteBuffer b=image.getPlanes()[0].getBuffer(); byte[] data=new byte[b.remaining()]; b.get(data); sendPacket('V',data); }
                finally { image.close(); }
            },cameraHandler);
            manager.openCamera(cameraId,new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice dev) {
                    camera=dev;
                    try { dev.createCaptureSession(Collections.singletonList(images.getSurface()),new CameraCaptureSession.StateCallback() {
                        @Override public void onConfigured(CameraCaptureSession configured) {
                            session=configured;
                            try { CaptureRequest.Builder req=dev.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW); req.addTarget(images.getSurface());
                                req.set(CaptureRequest.CONTROL_MODE,CaptureRequest.CONTROL_MODE_AUTO);
                                req.set(CaptureRequest.JPEG_QUALITY,(byte)82);
                                Integer orientation=manager.getCameraCharacteristics(cameraId).get(CameraCharacteristics.SENSOR_ORIENTATION);
                                if(orientation!=null) req.set(CaptureRequest.JPEG_ORIENTATION,orientation);
                                configured.setRepeatingRequest(req.build(),null,cameraHandler); }
                            catch(CameraAccessException e){showStatus("Falha ao iniciar a câmera");}
                        }
                        @Override public void onConfigureFailed(CameraCaptureSession failed){showStatus("A câmera não pôde ser configurada");}
                    },cameraHandler); } catch(CameraAccessException e){showStatus("Falha ao preparar a câmera");}
                }
                @Override public void onDisconnected(CameraDevice dev){dev.close();camera=null;}
                @Override public void onError(CameraDevice dev,int error){dev.close();camera=null;showStatus("Erro na câmera: "+error);}
            },cameraHandler);
        } catch(Exception e){showStatus("Não consegui abrir a câmera: "+e.getMessage());}
    }

    private void openMicrophone() {
        if(checkSelfPermission(Manifest.permission.RECORD_AUDIO)!=PackageManager.PERMISSION_GRANTED){showStatus("Permissão do microfone ausente");return;}
        int min=AudioRecord.getMinBufferSize(48000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);
        if(min<=0){showStatus("Microfone indisponível");return;}
        final AudioRecord rec=new AudioRecord(MediaRecorder.AudioSource.MIC,48000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT,Math.max(min,3840));
        recorder=rec; rec.startRecording();
        new Thread(() -> { byte[] data=new byte[1920]; while(mediaRunning && recorder==rec){int n=rec.read(data,0,data.length);if(n>0)sendPacket('A',Arrays.copyOf(data,n));} },"AndreyCam-Microphone").start();
    }

    private void sendPacket(char type,byte[] data) {
        DataOutputStream out=output; if(!connected||!mediaRunning||out==null)return;
        try { synchronized(out){out.writeByte(type);out.writeInt(data.length);out.write(data);out.flush();} }
        catch(Exception e){showStatus("Transmissão interrompida");disconnect();}
    }

    private void stopMedia() {
        mediaRunning=false; pendingMode=-1;
        if(recorder!=null){try{recorder.stop();}catch(Exception ignored){}recorder.release();recorder=null;}
        if(session!=null){session.close();session=null;} if(camera!=null){camera.close();camera=null;} if(images!=null){images.close();images=null;}
        if(cameraThread!=null){cameraThread.quitSafely();cameraThread=null;cameraHandler=null;}
    }

    private void disconnect() {
        connected=false;stopMedia();Socket old=socket;socket=null;output=null;if(old!=null)try{old.close();}catch(Exception ignored){}
        runOnUiThread(()->{if(connectButton!=null){connectButton.setText("Conectar");connectButton.setEnabled(true);}if(status!=null && status.getText().toString().startsWith("Transmitindo"))showStatus("Desconectado");});
    }

    private void findBackCamera() {
        try { CameraManager m=(CameraManager)getSystemService(Context.CAMERA_SERVICE);for(String id:m.getCameraIdList())if(m.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING)==CameraCharacteristics.LENS_FACING_BACK){cameraId=id;return;}
            String[] ids=m.getCameraIdList();if(ids.length>0)cameraId=ids[0]; } catch(Exception ignored){}
    }
    private void switchCamera() {
        try { CameraManager m=(CameraManager)getSystemService(Context.CAMERA_SERVICE);for(String id:m.getCameraIdList())if(!id.equals(cameraId)){cameraId=id;return;} }catch(Exception ignored){}
    }
    private void showStatus(String text){runOnUiThread(()->{if(status!=null)status.setText(text);});}
    @Override protected void onDestroy(){disconnect();super.onDestroy();}
}
