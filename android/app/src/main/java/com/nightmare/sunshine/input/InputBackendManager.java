package com.nightmare.sunshine.input;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.WindowManager;
import com.nightmare.sunshine_android.BuildConfig;
import com.topjohnwu.superuser.Shell;
import com.topjohnwu.superuser.ipc.RootService;
import java.util.HashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import rikka.shizuku.Shizuku;

/** Cached JNI facade. No Binder calls or privilege grant waits run on the UI thread. */
public final class InputBackendManager {
 public interface Listener {void onInputState(InputCapabilities state);void onBackendDeath(String reason);}
 private static final Object lock=new Object();
 private static final ExecutorService access=Executors.newSingleThreadExecutor();
 private static final Handler main=new Handler(Looper.getMainLooper());
 private static final CopyOnWriteArrayList<Listener> listeners=new CopyOnWriteArrayList<>();
 private static final HashMap<Integer,Sender> sessions=new HashMap<>();
 private static final class Sender {final IBinder owner=new Binder(){@Override protected boolean onTransact(int code,android.os.Parcel data,android.os.Parcel reply,int flags){if(code==IBinder.FIRST_CALL_TRANSACTION+100){int uid=Binder.getCallingUid();if(uid!=0&&uid!=2000)throw new SecurityException("Unknown worker UID");failure(data.readString(),true);return true;}return false;}};long sequence;}
 private static Context context;
 private static int mode=InputCapabilities.SHIZUKU,generation;
 private static IInputWorker worker;
 private static InputCapabilities capabilities=new InputCapabilities();
 private static Shizuku.UserServiceArgs shizukuArgs;
 private static ServiceConnection connection;
 private static final ProbeSessions probes=new ProbeSessions();
 private static boolean requesting;
 private static void unbind(ServiceConnection old,Shizuku.UserServiceArgs args){if(old==null)return;main.post(()->{try{if(args!=null)Shizuku.unbindUserService(args,old,true);else RootService.unbind(old);}catch(Exception ignored){}});}
 private static final Shizuku.OnRequestPermissionResultListener permission=(request,result)->{if(request==7341){if(result==PackageManager.PERMISSION_GRANTED)access.execute(InputBackendManager::bindShizuku);else failure("Shizuku permission denied",false);}};
 private InputBackendManager(){}
 public static void initialize(Context app){synchronized(lock){if(context!=null)return;context=app.getApplicationContext();mode=context.getSharedPreferences("input",0).getInt("mode",InputCapabilities.SHIZUKU);capabilities.backendKind=mode;capabilities.failureReason="Input access has not been requested";}Shizuku.addRequestPermissionResultListener(permission);Shizuku.addBinderDeadListener(()->{if(getMode()==InputCapabilities.SHIZUKU)failure("Shizuku server stopped",true);});}
 public static int getMode(){synchronized(lock){return mode;}}
 public static boolean isReady(){synchronized(lock){return worker!=null&&capabilities.failureReason==null&&(capabilities.supportedEventMask&31)==31;}}
 public static InputCapabilities getCapabilities(){synchronized(lock){return new InputCapabilities(capabilities);}}
 public static void addListener(Listener listener){listeners.addIfAbsent(listener);InputCapabilities state=getCapabilities();main.post(()->listener.onInputState(state));}
 public static void removeListener(Listener listener){listeners.remove(listener);}
 private static void publish(){InputCapabilities state=getCapabilities();main.post(()->{for(Listener l:listeners)l.onInputState(new InputCapabilities(state));});}
 private static void failure(String reason,boolean death){synchronized(lock){generation++;requesting=false;IInputWorker old=worker;ServiceConnection oldConnection=connection;Shizuku.UserServiceArgs oldArgs=shizukuArgs;connection=null;shizukuArgs=null;worker=null;sessions.clear();access.execute(()->{if(old!=null)try{old.destroy();}catch(Exception ignored){}unbind(oldConnection,oldArgs);});capabilities=new InputCapabilities();capabilities.backendKind=mode;capabilities.failureReason=reason;}publish();if(death)main.post(()->{for(Listener l:listeners)l.onBackendDeath(reason);});}
 public static void setMode(int value){if(value!=1&&value!=2)throw new IllegalArgumentException("Unknown input mode");synchronized(lock){if(mode==value)return;generation++;IInputWorker old=worker;ServiceConnection oldConnection=connection;Shizuku.UserServiceArgs oldArgs=shizukuArgs;connection=null;shizukuArgs=null;worker=null;sessions.clear();access.execute(()->{if(old!=null)try{old.destroy();}catch(Exception ignored){}if(oldConnection!=null)try{if(oldArgs!=null)Shizuku.unbindUserService(oldArgs,oldConnection,true);else main.post(()->RootService.unbind(oldConnection));}catch(Exception ignored){}});mode=value;context.getSharedPreferences("input",0).edit().putInt("mode",value).apply();}failure("Input mode changed; request access",true);}
 public static void requestAccess(){synchronized(lock){if(isReady()){publish();return;}if(requesting)return;requesting=true;}access.execute(()->{int chosen=getMode();if(chosen==InputCapabilities.ROOT){Shell.getShell(shell->{if(getMode()!=InputCapabilities.ROOT)return;if(!shell.isRoot()){failure("Root permission denied or unavailable",false);return;}bindRoot();});}else{try{if(!Shizuku.pingBinder()){failure("Start Shizuku before requesting access",false);return;}if(Shizuku.checkSelfPermission()!=PackageManager.PERMISSION_GRANTED){main.post(()->Shizuku.requestPermission(7341));return;}bindShizuku();}catch(Exception e){failure(e.toString(),false);}}});}
 private static ServiceConnection newConnection(){final int token; synchronized(lock){token=++generation;}
  return new ServiceConnection(){public void onServiceConnected(ComponentName name,IBinder binder){access.execute(()->{synchronized(lock){if(token!=generation)return;}try{IInputWorker candidate=IInputWorker.Stub.asInterface(binder);binder.linkToDeath(()->{synchronized(lock){if(token!=generation)return;}failure("Privileged input worker died",true);},0);
    WindowManager wm=(WindowManager)context.getSystemService(Context.WINDOW_SERVICE);Display d=wm.getDefaultDisplay();DisplayMetrics metrics=new DisplayMetrics();d.getRealMetrics(metrics);
    int probeId=probes.next();InputCapabilities state=candidate.openSession(probeId,new Binder(),d.getDisplayId(),metrics.widthPixels,metrics.heightPixels,d.getRotation()*90);candidate.closeSession(probeId);
    synchronized(lock){if(token!=generation)return;if(state.failureReason!=null){try{candidate.destroy();}catch(Exception ignored){}failure(state.failureReason,false);return;}capabilities=state;worker=candidate;requesting=false;}publish();
   }catch(Exception e){failure(e.toString(),false);}});}public void onServiceDisconnected(ComponentName name){synchronized(lock){if(token!=generation)return;}failure("Privileged input service disconnected",true);}};
 }
 private static void bindRoot(){connection=newConnection();Intent intent=new Intent(context,RootInputService.class);main.post(()->{try{RootService.bind(intent,connection);}catch(Exception e){failure("Root service launch failed: "+e,false);}});}
 private static void bindShizuku(){if(getMode()!=InputCapabilities.SHIZUKU)return;connection=newConnection();shizukuArgs=new Shizuku.UserServiceArgs(new ComponentName(context,ShizukuInputService.class)).daemon(false).tag("sunshine-input").version(BuildConfig.VERSION_CODE);try{Shizuku.bindUserService(shizukuArgs,connection);}catch(Exception e){failure("Shizuku service launch failed: "+e,false);}}
 private static InputCapabilities closed(String reason){InputCapabilities c=new InputCapabilities();c.backendKind=mode;c.failureReason=reason;return c;}
 private static IInputWorker require(){if(worker==null)throw new IllegalStateException("Input backend is not ready");return worker;}
 public static InputCapabilities openSession(int id,int display,int width,int height,int rotation){synchronized(lock){if(sessions.containsKey(id))return closed("Duplicate input session");try{Sender s=new Sender();InputCapabilities c=require().openSession(id,s.owner,display,width,height,rotation);if(c.failureReason==null)sessions.put(id,s);return c;}catch(Exception e){return closed(e.toString());}}}
 public static void submitBatch(int id,InputEventData[] events,int count){synchronized(lock){Sender s=sessions.get(id);if(s==null)return;if(count<1||count>32||events==null||count>events.length)throw new IllegalArgumentException("Invalid input batch");InputEventData[] snapshot=new InputEventData[count];for(int i=0;i<count;i++)snapshot[i]=new InputEventData(events[i]);try{require().submitBatch(id,++s.sequence,snapshot);}catch(RemoteException e){failure("Input worker unavailable",true);}}}
 public static InputResult submitText(int id,String text){synchronized(lock){Sender s=sessions.get(id);if(s==null)return new InputResult(InputResult.SESSION_CLOSED,"Session closed");try{return require().submitText(id,++s.sequence,text);}catch(Exception e){return new InputResult(InputResult.DEVICE_UNAVAILABLE,e.toString());}}}
 public static InputResult createController(int id,int controller){synchronized(lock){if(!sessions.containsKey(id))return new InputResult(InputResult.SESSION_CLOSED,"Session closed");try{return require().createController(id,controller);}catch(Exception e){return new InputResult(InputResult.DEVICE_UNAVAILABLE,e.toString());}}}
 public static void removeController(int id,int controller){synchronized(lock){if(!sessions.containsKey(id))return;try{require().removeController(id,controller);}catch(Exception e){failure(e.toString(),true);}}}
 public static void resetSession(int id){synchronized(lock){Sender s=sessions.get(id);if(s==null)return;try{require().resetSession(id,++s.sequence);}catch(Exception e){failure(e.toString(),true);}}}
 public static void closeSession(int id){synchronized(lock){if(sessions.remove(id)==null)return;try{require().closeSession(id);}catch(Exception e){failure(e.toString(),true);}}}
}
