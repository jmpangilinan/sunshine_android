package com.nightmare.sunshine.input;

import android.content.Context;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ConcurrentHashMap;

/** Binder entrypoints authenticate before posting; resets invalidate queued batches immediately. */
public final class InputWorker extends IInputWorker.Stub {
 private final int appUid,kind;
 private final HandlerThread thread=new HandlerThread("sunshine-input");
 private final Handler queue;
 private final Map<Integer,Session> sessions=new HashMap<>();
 private final java.util.HashSet<Integer> closedIds=new java.util.HashSet<>();
 private final SequenceGate floors=new SequenceGate();
 private volatile boolean destroyed;
 private static final class Session {int uid;long sequence;IBinder owner;IBinder.DeathRecipient death;AndroidInputInjector injector;VirtualInputDevices devices;final MouseRoute mouse=new MouseRoute();}
 public InputWorker(Context context,int kind) {
  this.kind=kind;
  try {appUid=context.getPackageManager().getApplicationInfo("com.nightmare.sunshine_android",0).uid;}
  catch(Exception e){throw new SecurityException("Cannot resolve owning Sunshine application",e);}
  thread.start();queue=new Handler(thread.getLooper());
 }
 static Context systemContext(){try{Class<?> at=Class.forName("android.app.ActivityThread");Object instance=at.getMethod("systemMain").invoke(null);return (Context)at.getMethod("getSystemContext").invoke(instance);}catch(Exception e){throw new IllegalStateException("Cannot obtain package-manager context",e);}}
 private void authenticate(){if(Binder.getCallingUid()!=appUid)throw new SecurityException("Only the Sunshine application owns this worker");if(destroyed)throw new IllegalStateException("Worker destroyed");}
 private <T>T barrier(java.util.concurrent.Callable<T> task){FutureTask<T> future=new FutureTask<>(task);queue.post(future);try{return future.get();}catch(Exception e){Throwable cause=e.getCause();if(cause instanceof RuntimeException)throw (RuntimeException)cause;throw new IllegalStateException(e);}}
 private Session session(int id){Session s=sessions.get(id);if(s==null)throw new IllegalStateException("Input session closed");return s;}
 private static InputCapabilities unavailable(int kind,String reason){InputCapabilities c=new InputCapabilities();c.uid=Process.myUid();c.backendKind=kind;c.failureReason=reason;return c;}
 @Override public InputCapabilities openSession(int id,IBinder owner,int display,int width,int height,int rotation){authenticate();final int uid=Binder.getCallingUid();if(owner==null||id<=0||width<=0||height<=0||display<0)return unavailable(kind,"Invalid session geometry or owner");
  return barrier(()->{if(sessions.containsKey(id)||closedIds.contains(id))return unavailable(kind,"Duplicate or previously closed session");Session s=new Session();s.uid=uid;s.owner=owner;
   try{s.injector=new AndroidInputInjector(display,width,height);s.injector.probe();s.devices=new VirtualInputDevices();s.devices.openMouse();s.devices.openKeyboard();s.injector.setDevices(s.devices);s.death=()->{floors.close(id);queue.post(()->close(id));};owner.linkToDeath(s.death,0);sessions.put(id,s);floors.open(id);
    InputCapabilities c=new InputCapabilities();c.uid=Process.myUid();c.backendKind=kind;c.supportedEventMask=63;c.relativeMouse=s.devices.hasRelativeMouse();c.multitouch=true;c.pen=s.injector.hasPen();if(c.pen)c.supportedEventMask|=64;c.mappedText=true;
    InputResult controller=s.devices.createController(15);
    if(controller.code==InputResult.OK){s.devices.removeController(15);c.maxControllers=16;c.supportedEventMask|=128;}
    return c;
   }catch(Exception e){sessions.remove(id);floors.close(id);if(s.death!=null)owner.unlinkToDeath(s.death,0);if(s.devices!=null)s.devices.close();return unavailable(kind,AndroidInputInjector.failureMessage(e));}
  });
 }
 @Override public void submitBatch(int id,long sequence,InputEventData[] events){authenticate();if(events==null||events.length==0||events.length>32)throw new IllegalArgumentException("Batch must contain 1..32 events");InputEventData[] copy=new InputEventData[events.length];for(int i=0;i<events.length;i++){if(events[i]==null)throw new IllegalArgumentException("Null event");copy[i]=new InputEventData(events[i]);}
  queue.post(()->{Session s=sessions.get(id);if(s==null||!floors.accepts(id,sequence)||sequence<=s.sequence)return;
   try{if(!s.injector.validBatch(copy))throw new IllegalArgumentException("Malformed event or pointer transition");s.sequence=sequence;for(InputEventData e:copy){if(!floors.accepts(id,sequence))break;apply(s,e);}}
   catch(RuntimeException e){floors.close(id);try{android.os.Parcel notification=android.os.Parcel.obtain();try{notification.writeString(e.toString());s.owner.transact(IBinder.FIRST_CALL_TRANSACTION+100,notification,null,IBinder.FLAG_ONEWAY);}finally{notification.recycle();}}catch(RemoteException ignored){}close(id);}
  });
 }
 private void apply(Session s,InputEventData e){VirtualInputDevices d=s.devices;switch(e.kind){
  case InputEventData.ABS_MOUSE:
   int held=s.mouse.beginAbsolute();
   if(d.hasRelativeMouse()){for(int button=1;button<=5;button++)if((held&(1<<(button-1)))!=0&&!d.mouseButton(button,true))throw new IllegalStateException(d.failureReason());}
   if(held!=0&&d.hasRelativeMouse()){
    s.injector.positionMouse(e.x,e.y);
    for(int button=1;button<=5;button++)if((held&(1<<(button-1)))!=0){InputEventData press=new InputEventData();press.kind=InputEventData.MOUSE_BUTTON;press.button=button;s.injector.apply(press);}
   }
   s.injector.apply(e);return;
  case InputEventData.REL_MOUSE:if(s.mouse.useVirtual(d.hasRelativeMouse())){if(!d.moveMouse(Math.round(e.deltaX),Math.round(e.deltaY)))throw new IllegalStateException(d.failureReason());return;}break;
  case InputEventData.MOUSE_BUTTON:s.mouse.button(e.button,e.release);if(s.mouse.useVirtual(d.hasRelativeMouse())){if(!d.mouseButton(e.button,e.release))throw new IllegalStateException(d.failureReason());return;}break;
  case InputEventData.SCROLL:if(s.mouse.useVirtual(d.hasRelativeMouse())){if(!d.scroll(Math.round(e.verticalScroll),Math.round(e.horizontalScroll)))throw new IllegalStateException(d.failureReason());return;}break;
  case InputEventData.CONTROLLER_STATE:if(!d.controller(e.controllerId,e.controllerButtons,e.leftTrigger,e.rightTrigger,e.leftStickX,e.leftStickY,e.rightStickX,e.rightStickY))throw new IllegalStateException(d.failureReason());return;
  default:break;
 }s.injector.apply(e);}
 @Override public InputResult submitText(int id,long sequence,String text){authenticate();return barrier(()->{Session s=sessions.get(id);if(s==null)return new InputResult(InputResult.SESSION_CLOSED,"Session closed");if(!floors.accepts(id,sequence)||sequence<=s.sequence)return new InputResult(InputResult.INVALID_ARGUMENT,"Stale sequence");s.sequence=sequence;return s.injector.text(text);});}
 @Override public InputResult createController(int id,int controller){authenticate();return barrier(()->session(id).devices.createController(controller));}
 @Override public void removeController(int id,int controller){authenticate();barrier(()->{session(id).devices.removeController(controller);return null;});}
 @Override public void resetSession(int id,long sequence){authenticate();floors.reset(id,sequence);barrier(()->{Session s=sessions.get(id);if(s!=null){s.sequence=Math.max(s.sequence,sequence);try{s.injector.reset();}finally{s.devices.reset();s.mouse.reset();}}return null;});}
 @Override public void closeSession(int id){authenticate();floors.close(id);barrier(()->{close(id);return null;});}
 private void close(int id){Session s=sessions.remove(id);closedIds.add(id);floors.close(id);if(s==null)return;s.owner.unlinkToDeath(s.death,0);try{s.injector.reset();}catch(RuntimeException ignored){}finally{s.devices.close();}}
 @Override public void destroy(){authenticate();shutdown();}
 public void shutdown(){if(destroyed)return;destroyed=true;floors.clear();barrier(()->{for(Integer id:sessions.keySet().toArray(new Integer[0]))close(id);return null;});thread.quitSafely();}
}
