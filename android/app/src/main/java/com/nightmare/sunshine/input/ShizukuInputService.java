package com.nightmare.sunshine.input;
import android.os.IBinder;
public final class ShizukuInputService extends IInputWorker.Stub {
 private final InputWorker worker;
 public ShizukuInputService(){worker=new InputWorker(InputWorker.systemContext(),InputCapabilities.SHIZUKU);}
@Override public InputCapabilities openSession(int id, IBinder owner, int display, int width, int height, int rotation){return worker.openSession(id,owner,display,width,height,rotation);}
@Override public void submitBatch(int id,long sequence,InputEventData[] events){worker.submitBatch(id,sequence,events);}
@Override public InputResult submitText(int id,long sequence,String text){return worker.submitText(id,sequence,text);}
@Override public InputResult createController(int id,int controller){return worker.createController(id,controller);}
@Override public void removeController(int id,int controller){worker.removeController(id,controller);}
@Override public void resetSession(int id,long sequence){worker.resetSession(id,sequence);}
@Override public void closeSession(int id){worker.closeSession(id);}
@Override public void destroy(){worker.destroy();}
}
