package com.nightmare.sunshine.input;
import android.content.Intent;
import android.os.IBinder;
import com.topjohnwu.superuser.ipc.RootService;
public final class RootInputService extends RootService {
 private InputWorker worker;
 @Override public IBinder onBind(Intent intent){if(worker==null)worker=new InputWorker(this,InputCapabilities.ROOT);return worker;}
 @Override public void onDestroy(){if(worker!=null)worker.shutdown();super.onDestroy();}
}
