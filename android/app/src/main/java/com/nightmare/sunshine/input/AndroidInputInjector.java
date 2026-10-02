package com.nightmare.sunshine.input;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.LinkedHashMap;
import java.util.Map;

/** All state is confined to the worker's ordered thread. Coordinates are physical display pixels. */
public final class AndroidInputInjector {
 private final Object manager;
 private final Method inject, setDisplay, setButton;
 private final int displayId, width, height;
 private final LinkedHashMap<Integer,InputEventData> pointers=new LinkedHashMap<>();
 private final LinkedHashMap<Integer,KeyEvent> keys=new LinkedHashMap<>();
 private long touchDown,mouseDown;
 private int mouseButtons;
 private float mouseX,mouseY;
 private final PointerTracker pointerSlots=new PointerTracker();
 private VirtualInputDevices devices;
 public void setDevices(VirtualInputDevices devices){this.devices=devices;}
 public void positionMouse(float x,float y){mouseX=clip(x,width);mouseY=clip(y,height);}
 private int penButtonState;
 private int penActionButton;
 private boolean penAvailable;
 private InputEventData penHover;
 public boolean hasPen(){return penAvailable;}
 public boolean validBatch(InputEventData[] events){
  java.util.HashSet<Integer> active=new java.util.HashSet<>(pointers.keySet());
  for(InputEventData e:events){if(!valid(e))return false;if(e.kind!=InputEventData.TOUCH&&e.kind!=InputEventData.PEN)continue;
   if(e.action==4||e.action==7){active.clear();continue;}
   if(e.action==1){if(!active.add(e.pointerId)||active.size()>10)return false;}
   if(e.action==2){if(!active.remove(e.pointerId))return false;}
   if(e.action==3&&!active.contains(e.pointerId))return false;
  }return true;
 }
 // Constructed only in libsu/Shizuku root or shell workers, not the application process.
 // Shizuku's UserService guide explicitly permits non-SDK APIs in this process:
 // https://github.com/RikkaApps/Shizuku-API#userservice
 // Keep runtime probe failures visible; this exemption does not imply OEM injection permission.
 @android.annotation.SuppressLint("BlockedPrivateApi")
 public AndroidInputInjector(int displayId,int width,int height) throws Exception {
  int uid=android.os.Process.myUid();
  if(uid!=0&&uid!=2000)throw new SecurityException("Input injection requires a root or shell worker process");
  this.displayId=displayId; this.width=width; this.height=height;
  Class<?> type;
  try {type=Class.forName("android.hardware.input.InputManagerGlobal");} catch(ClassNotFoundException e){type=Class.forName("android.hardware.input.InputManager");}
  manager=type.getDeclaredMethod("getInstance").invoke(null);
  inject=type.getMethod("injectInputEvent",InputEvent.class,int.class);
  setDisplay=InputEvent.class.getDeclaredMethod("setDisplayId",int.class); setDisplay.setAccessible(true);
  setButton=MotionEvent.class.getDeclaredMethod("setActionButton",int.class); setButton.setAccessible(true);
 }
 private boolean send(InputEvent event,int mode) {
  try {
   if(event instanceof KeyEvent&&mode==0&&devices!=null&&devices.hasKeyboard()){
    KeyEvent key=(KeyEvent)event;int linux=LinuxKeyMap.fromAndroid(key.getKeyCode());
    if(linux>=0){if(!devices.key(linux,key.getAction()==KeyEvent.ACTION_UP))throw new IllegalStateException(devices.failureReason());return true;}
   }
   setDisplay.invoke(event,displayId);boolean ok=Boolean.TRUE.equals(inject.invoke(manager,event,mode));if(!ok&&mode==0)throw new IllegalStateException("injectInputEvent returned false");return ok;
  }
  catch(Exception e){Throwable cause=injectionCause(e);throw new IllegalStateException("Input injection failed for "+eventDescription(event)+": "+failureMessage(cause),cause);}
  finally {if(event instanceof MotionEvent)((MotionEvent)event).recycle();}
 }
 static String failureMessage(Throwable failure){Throwable cause=injectionCause(failure);return cause.getClass().getSimpleName()+": "+(cause.getMessage()==null?cause.toString():cause.getMessage());}
 private static Throwable injectionCause(Throwable failure){while(failure instanceof InvocationTargetException&&failure.getCause()!=null)failure=failure.getCause();return failure;}
 private static String eventDescription(InputEvent event){return event instanceof MotionEvent?"MotionEvent "+MotionEvent.actionToString(((MotionEvent)event).getAction()):"KeyEvent action="+((KeyEvent)event).getAction();}
 private static void requireProbeEvent(InputEvent event,java.util.function.Predicate<InputEvent> sender){String description=eventDescription(event);if(!sender.test(event))throw new IllegalStateException("Input readiness probe rejected "+description+": injectInputEvent returned false");}
 static void probeTouch(int width,int height,java.util.function.Predicate<InputEvent> sender){
  long downTime=SystemClock.uptimeMillis();
  MotionEvent.PointerProperties p=new MotionEvent.PointerProperties();p.id=0;p.toolType=MotionEvent.TOOL_TYPE_FINGER;
  MotionEvent.PointerCoords c=new MotionEvent.PointerCoords();c.x=(width-1)/2f;c.y=(height-1)/2f;c.pressure=1;
  MotionEvent.PointerProperties[] properties={p};MotionEvent.PointerCoords[] coordinates={c};
  boolean downAccepted=false;
  try{
   requireProbeEvent(MotionEvent.obtain(downTime,downTime,MotionEvent.ACTION_DOWN,1,properties,coordinates,0,0,1,1,KeyCharacterMap.VIRTUAL_KEYBOARD,0,InputDevice.SOURCE_TOUCHSCREEN,0),sender);
   downAccepted=true;
  }finally{
   if(downAccepted)requireProbeEvent(MotionEvent.obtain(downTime,SystemClock.uptimeMillis(),MotionEvent.ACTION_CANCEL,1,properties,coordinates,0,0,1,1,KeyCharacterMap.VIRTUAL_KEYBOARD,0,InputDevice.SOURCE_TOUCHSCREEN,0),sender);
  }
 }
 private static final class HoverCleanupException extends IllegalStateException {
  HoverCleanupException(RuntimeException cause){super("Input readiness hover cleanup failed",cause);}
 }
 static void probeHover(int toolType,int source,java.util.function.Predicate<InputEvent> sender){
  long downTime=SystemClock.uptimeMillis();
  MotionEvent.PointerProperties p=new MotionEvent.PointerProperties();p.id=0;p.toolType=toolType;
  MotionEvent.PointerCoords c=new MotionEvent.PointerCoords();
  MotionEvent.PointerProperties[] properties={p};MotionEvent.PointerCoords[] coordinates={c};
  RuntimeException failure=null;
  try{
   requireProbeEvent(MotionEvent.obtain(downTime,downTime,MotionEvent.ACTION_HOVER_ENTER,1,properties,coordinates,0,0,1,1,KeyCharacterMap.VIRTUAL_KEYBOARD,0,source,0),sender);
   requireProbeEvent(MotionEvent.obtain(downTime,SystemClock.uptimeMillis(),MotionEvent.ACTION_HOVER_MOVE,1,properties,coordinates,0,0,1,1,KeyCharacterMap.VIRTUAL_KEYBOARD,0,source,0),sender);
  }catch(RuntimeException e){failure=e;throw e;}
  finally{
   // Even a rejected/throwing injection may have reached the dispatcher. Always neutralize it.
   try{requireProbeEvent(MotionEvent.obtain(downTime,SystemClock.uptimeMillis(),MotionEvent.ACTION_HOVER_EXIT,1,properties,coordinates,0,0,1,1,KeyCharacterMap.VIRTUAL_KEYBOARD,0,source,0),sender);}
   catch(RuntimeException e){HoverCleanupException cleanup=new HoverCleanupException(e);if(failure!=null)cleanup.addSuppressed(failure);throw cleanup;}
  }
 }
 public void probe() {
  long now=SystemClock.uptimeMillis();
  probeHover(MotionEvent.TOOL_TYPE_MOUSE,InputDevice.SOURCE_MOUSE,event->send(event,1));
  requireProbeEvent(new KeyEvent(now,now,KeyEvent.ACTION_UP,KeyEvent.KEYCODE_SHIFT_LEFT,0,0,KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD),event->send(event,1));
  probeTouch(width,height,event->send(event,1));
  penAvailable=false;
  try{probeHover(MotionEvent.TOOL_TYPE_STYLUS,InputDevice.SOURCE_STYLUS,event->send(event,1));penAvailable=true;}
  catch(HoverCleanupException e){throw e;}
  catch(RuntimeException ignored){penAvailable=false;}
 }
 private float clip(float v,int extent){return Math.max(0,Math.min(extent-1,v));}
 private static boolean finite(float v){return !Float.isNaN(v)&&!Float.isInfinite(v);}
 public boolean valid(InputEventData e) {
  if(e==null||e.kind<1||e.kind>8)return false;
  if(!finite(e.x)||!finite(e.y)||!finite(e.deltaX)||!finite(e.deltaY)||!finite(e.pressure)||!finite(e.contactMajor)||!finite(e.contactMinor)||!finite(e.verticalScroll)||!finite(e.horizontalScroll))return false;
  if(e.kind==InputEventData.KEY)return mapKey(e.keyCode,e.modifiers)!=KeyEvent.KEYCODE_UNKNOWN;
  if(e.kind==InputEventData.MOUSE_BUTTON)return e.button>=1&&e.button<=5;
  if(e.kind==InputEventData.PEN&&!penAvailable)return false;
  if(e.kind==InputEventData.TOUCH||e.kind==InputEventData.PEN)return e.action>=0&&e.action<=7&&e.pointerId>=0&&e.pressure>=0&&((e.action==0||e.action==6)||e.pressure<=1)&&e.contactMajor>=0&&e.contactMinor>=0&&e.toolType>=0&&e.toolType<=2&&(e.penButtons&~7)==0;
  if(e.kind==InputEventData.CONTROLLER_STATE)return e.controllerId>=0&&e.controllerId<16&&e.leftTrigger>=0&&e.leftTrigger<=255&&e.rightTrigger>=0&&e.rightTrigger<=255&&e.leftStickX>=-32768&&e.leftStickX<=32767&&e.leftStickY>=-32768&&e.leftStickY<=32767&&e.rightStickX>=-32768&&e.rightStickX<=32767&&e.rightStickY>=-32768&&e.rightStickY<=32767;
  return true;
 }
 public void apply(InputEventData e) {
  if(!valid(e))throw new IllegalArgumentException("Malformed input event");
  switch(e.kind){
   case InputEventData.KEY:key(e);break;
   case InputEventData.TOUCH:case InputEventData.PEN:touch(e);break;
   case InputEventData.ABS_MOUSE:mouseX=clip(e.x,width);mouseY=clip(e.y,height);mouse(mouseButtons==0?MotionEvent.ACTION_HOVER_MOVE:MotionEvent.ACTION_MOVE,0,0,0);break;
   case InputEventData.REL_MOUSE:mouseX=clip(mouseX+e.deltaX,width);mouseY=clip(mouseY+e.deltaY,height);mouse(mouseButtons==0?MotionEvent.ACTION_HOVER_MOVE:MotionEvent.ACTION_MOVE,0,0,0);break;
   case InputEventData.SCROLL:mouse(MotionEvent.ACTION_SCROLL,0,e.verticalScroll/120f,e.horizontalScroll/120f);break;
   case InputEventData.MOUSE_BUTTON:
    int b=button(e.button),old=mouseButtons;mouseButtons=e.release?old&~b:old|b;
    if(old==mouseButtons)return;
    if(old==0&&!e.release){mouseDown=SystemClock.uptimeMillis();mouse(MotionEvent.ACTION_DOWN,0,0,0);}
    mouse(e.release?MotionEvent.ACTION_BUTTON_RELEASE:MotionEvent.ACTION_BUTTON_PRESS,b,0,0);
    if(mouseButtons==0)mouse(MotionEvent.ACTION_UP,0,0,0);break;
   default:throw new IllegalArgumentException("Device event sent to Binder injector");
  }
 }
 private static int button(int b){return b==1?MotionEvent.BUTTON_PRIMARY:b==2?MotionEvent.BUTTON_TERTIARY:b==3?MotionEvent.BUTTON_SECONDARY:b==4?MotionEvent.BUTTON_BACK:MotionEvent.BUTTON_FORWARD;}
 private void mouse(int action,int actionButton,float vertical,float horizontal){
  MotionEvent.PointerProperties p=new MotionEvent.PointerProperties();p.id=0;p.toolType=MotionEvent.TOOL_TYPE_MOUSE;
  MotionEvent.PointerCoords c=new MotionEvent.PointerCoords();c.x=mouseX;c.y=mouseY;c.pressure=mouseButtons==0?0:1;c.setAxisValue(MotionEvent.AXIS_VSCROLL,vertical);c.setAxisValue(MotionEvent.AXIS_HSCROLL,horizontal);
  MotionEvent ev=MotionEvent.obtain(mouseDown,SystemClock.uptimeMillis(),action,1,new MotionEvent.PointerProperties[]{p},new MotionEvent.PointerCoords[]{c},0,mouseButtons,1,1,0,0,InputDevice.SOURCE_MOUSE,0);
  try{if(actionButton!=0)setButton.invoke(ev,actionButton);}catch(Exception ex){ev.recycle();throw new IllegalStateException(ex);} send(ev,0);
 }
 private void touch(InputEventData e){
  boolean pen=e.kind==InputEventData.PEN;
  if(e.action==7||e.action==4){try{cancelTouch();}finally{if(pen)exitPenHover();}return;}
  if(e.action==0||e.action==6||e.action==5){
   if(!pen&&e.action!=0&&e.action!=6)throw new IllegalArgumentException("Finger button event");
   if(e.action==5){int old=penButtonState;penButtonState=e.penButtons;for(int bit=1;bit<=2;bit<<=1){if(((old^penButtonState)&bit)==0)continue;penActionButton=bit==1?MotionEvent.BUTTON_STYLUS_PRIMARY:MotionEvent.BUTTON_STYLUS_SECONDARY;motion((penButtonState&bit)!=0?MotionEvent.ACTION_BUTTON_PRESS:MotionEvent.ACTION_BUTTON_RELEASE,new InputEventData[]{e},0,true);}return;}
   if(pen){
    if(e.action==6){exitPenHover();return;}
    if(penHover!=null&&(penHover.pointerId!=e.pointerId||penHover.toolType!=e.toolType))exitPenHover();
    boolean entering=penHover==null;penHover=new InputEventData(e);
    if(entering)motion(MotionEvent.ACTION_HOVER_ENTER,new InputEventData[]{penHover},0,true);
    motion(MotionEvent.ACTION_HOVER_MOVE,new InputEventData[]{penHover},0,true);return;
   }
   motion(e.action==6?MotionEvent.ACTION_HOVER_EXIT:MotionEvent.ACTION_HOVER_MOVE,new InputEventData[]{e},0,false);return;
  }
  if(pen&&e.action==1)exitPenHover();
  boolean exists=pointers.containsKey(e.pointerId);
  if(!pointers.isEmpty()&&(pointers.values().iterator().next().kind==InputEventData.PEN)!=pen)throw new IllegalArgumentException("Mixed finger and pen gesture");
  if(e.action==1){if(exists||pointers.size()>=10)throw new IllegalArgumentException("Invalid pointer down");if(pointers.isEmpty())touchDown=SystemClock.uptimeMillis();pointerSlots.down(e.pointerId);pointers.put(e.pointerId,new InputEventData(e));}
  else {if(!exists)throw new IllegalArgumentException("Pointer is not down");pointers.put(e.pointerId,new InputEventData(e));}
  InputEventData[] values=pointers.values().toArray(new InputEventData[0]);int index=0;for(int i=0;i<values.length;i++)if(values[i].pointerId==e.pointerId)index=i;
  int action=e.action==1?(values.length==1?MotionEvent.ACTION_DOWN:MotionEvent.ACTION_POINTER_DOWN|(index<<8)):e.action==2?(values.length==1?MotionEvent.ACTION_UP:MotionEvent.ACTION_POINTER_UP|(index<<8)):MotionEvent.ACTION_MOVE;
  motion(action,values,index,pen);if(e.action==2){pointers.remove(e.pointerId);pointerSlots.up(e.pointerId);}
 }
 private void motion(int action,InputEventData[] values,int index,boolean pen){
  MotionEvent.PointerProperties[] pp=new MotionEvent.PointerProperties[values.length];MotionEvent.PointerCoords[] cc=new MotionEvent.PointerCoords[values.length];
  int buttons=0;
  for(int i=0;i<values.length;i++){InputEventData e=values[i];pp[i]=new MotionEvent.PointerProperties();pp[i].id=pointerSlots.contains(e.pointerId)?pointerSlots.slot(e.pointerId):0;pp[i].toolType=e.kind==InputEventData.PEN?(e.toolType==2?MotionEvent.TOOL_TYPE_ERASER:MotionEvent.TOOL_TYPE_STYLUS):MotionEvent.TOOL_TYPE_FINGER;
   cc[i]=new MotionEvent.PointerCoords();cc[i].x=clip(e.x,width);cc[i].y=clip(e.y,height);boolean hover=e.action==0||e.action==6;cc[i].pressure=hover?0:e.pressure;if(hover)cc[i].setAxisValue(MotionEvent.AXIS_DISTANCE,e.pressure);cc[i].touchMajor=e.contactMajor;cc[i].touchMinor=e.contactMinor;cc[i].orientation=e.rotation==65535?0:(float)Math.toRadians(e.rotation);cc[i].setAxisValue(MotionEvent.AXIS_TILT,e.tilt==255?0:(float)Math.toRadians(e.tilt));if((e.penButtons&1)!=0)buttons|=MotionEvent.BUTTON_STYLUS_PRIMARY;if((e.penButtons&2)!=0)buttons|=MotionEvent.BUTTON_STYLUS_SECONDARY;
  }
  MotionEvent event=MotionEvent.obtain(touchDown,SystemClock.uptimeMillis(),action,values.length,pp,cc,0,buttons,1,1,pen?KeyCharacterMap.VIRTUAL_KEYBOARD:0,0,pen?InputDevice.SOURCE_STYLUS:InputDevice.SOURCE_TOUCHSCREEN,0);
  if(action==MotionEvent.ACTION_BUTTON_PRESS||action==MotionEvent.ACTION_BUTTON_RELEASE){try{setButton.invoke(event,penActionButton);}catch(Exception e){event.recycle();throw new IllegalStateException(e);}}
  send(event,0);
 }
 private void exitPenHover(){
  if(penHover==null)return;
  InputEventData hover=penHover;hover.action=6;
  try{motion(MotionEvent.ACTION_HOVER_EXIT,new InputEventData[]{hover},0,true);}finally{penHover=null;}
 }
 private void cancelTouch(){if(!pointers.isEmpty()){motion(MotionEvent.ACTION_CANCEL,pointers.values().toArray(new InputEventData[0]),0,pointers.values().iterator().next().kind==InputEventData.PEN);pointers.clear();pointerSlots.cancel();}}
 private void key(InputEventData e){int code=mapKey(e.keyCode,e.modifiers);int identity=(e.keyCode<<16)|((e.modifiers&16)<<8)|(e.flags&255);long now=SystemClock.uptimeMillis();KeyEvent old=keys.get(identity);
  if(e.release){if(old==null)return;keys.remove(identity);send(new KeyEvent(old.getDownTime(),now,KeyEvent.ACTION_UP,old.getKeyCode(),0,old.getMetaState(),KeyCharacterMap.VIRTUAL_KEYBOARD,old.getScanCode(),0,InputDevice.SOURCE_KEYBOARD),0);}
  else {int repeat=old==null?0:old.getRepeatCount()+1;KeyEvent event=new KeyEvent(old==null?now:old.getDownTime(),now,KeyEvent.ACTION_DOWN,code,repeat,meta(),KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD);keys.put(identity,event);send(event,0);}}
 private int meta(){int m=0;for(KeyEvent e:keys.values()){switch(e.getKeyCode()){case KeyEvent.KEYCODE_SHIFT_LEFT:case KeyEvent.KEYCODE_SHIFT_RIGHT:m|=KeyEvent.META_SHIFT_ON;break;case KeyEvent.KEYCODE_CTRL_LEFT:case KeyEvent.KEYCODE_CTRL_RIGHT:m|=KeyEvent.META_CTRL_ON;break;case KeyEvent.KEYCODE_ALT_LEFT:case KeyEvent.KEYCODE_ALT_RIGHT:m|=KeyEvent.META_ALT_ON;break;case KeyEvent.KEYCODE_META_LEFT:case KeyEvent.KEYCODE_META_RIGHT:m|=KeyEvent.META_META_ON;break;}}return m;}
 public InputResult text(String text){if(text==null)return new InputResult(InputResult.INVALID_ARGUMENT,"Missing text");KeyEvent[] events=KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(text.toCharArray());if(events==null)return new InputResult(InputResult.UNSUPPORTED_TEXT,"Text is not mapped by the virtual keyboard");
  LinkedHashMap<Integer,KeyEvent> held=new LinkedHashMap<>(keys);for(KeyEvent k:held.values())send(new KeyEvent(k.getDownTime(),SystemClock.uptimeMillis(),KeyEvent.ACTION_UP,k.getKeyCode(),0,0,KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD),0);
  try{for(KeyEvent k:events)send(k,0);}finally{for(KeyEvent k:held.values())send(new KeyEvent(k.getDownTime(),SystemClock.uptimeMillis(),KeyEvent.ACTION_DOWN,k.getKeyCode(),0,k.getMetaState(),KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD),0);}return new InputResult(InputResult.OK,null);
 }
 public void reset(){
  RuntimeException failure=null;
  try{cancelTouch();}catch(RuntimeException e){failure=e;}finally{pointers.clear();pointerSlots.cancel();penButtonState=0;}
  try{exitPenHover();}catch(RuntimeException e){failure=e;}
  for(KeyEvent e:keys.values()){try{send(new KeyEvent(e.getDownTime(),SystemClock.uptimeMillis(),KeyEvent.ACTION_UP,e.getKeyCode(),0,0,KeyCharacterMap.VIRTUAL_KEYBOARD,0,0,InputDevice.SOURCE_KEYBOARD),0);}catch(RuntimeException ex){failure=ex;}}
  keys.clear();
  if(mouseButtons!=0){int held=mouseButtons;mouseButtons=0;try{for(int b:new int[]{MotionEvent.BUTTON_PRIMARY,MotionEvent.BUTTON_TERTIARY,MotionEvent.BUTTON_SECONDARY,MotionEvent.BUTTON_BACK,MotionEvent.BUTTON_FORWARD})if((held&b)!=0)mouse(MotionEvent.ACTION_BUTTON_RELEASE,b,0,0);mouse(MotionEvent.ACTION_UP,0,0,0);}catch(RuntimeException e){failure=e;}}
  if(failure!=null)throw failure;
 }
 public static int mapKey(int vk,int modifiers){boolean ext=(modifiers&16)!=0;
  if(vk>=0x41&&vk<=0x5a)return KeyEvent.KEYCODE_A+vk-0x41;if(vk>=0x30&&vk<=0x39)return KeyEvent.KEYCODE_0+vk-0x30;if(vk>=0x70&&vk<=0x7b)return KeyEvent.KEYCODE_F1+vk-0x70;if(vk>=0x60&&vk<=0x69)return KeyEvent.KEYCODE_NUMPAD_0+vk-0x60;
  switch(vk){case 8:return 67;case 9:return 61;case 13:return ext?160:66;case 16:return ext?60:59;case 17:return ext?114:113;case 18:return ext?58:57;case 19:return 121;case 20:return 115;case 27:return 111;case 32:return 62;case 33:return 92;case 34:return 93;case 35:return 123;case 36:return 122;case 37:return 21;case 38:return 19;case 39:return 22;case 40:return 20;case 44:return 120;case 45:return 124;case 46:return 112;case 91:return 117;case 92:return 118;case 93:return 82;case 106:return 155;case 107:return 157;case 109:return 156;case 110:return 158;case 111:return 154;case 144:return 143;case 145:return 116;case 160:return 59;case 161:return 60;case 162:return 113;case 163:return 114;case 164:return 57;case 165:return 58;case 173:return 164;case 174:return 25;case 175:return 24;case 176:return 87;case 177:return 88;case 178:return 86;case 179:return 85;case 186:return 74;case 187:return 70;case 188:return 55;case 189:return 69;case 190:return 56;case 191:return 76;case 192:return 68;case 219:return 71;case 220:return 73;case 221:return 72;case 222:return 75;default:return KeyEvent.KEYCODE_UNKNOWN;}
 }
}
