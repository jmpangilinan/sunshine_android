package com.nightmare.sunshine.input;

import android.app.Instrumentation;
import android.app.UiAutomation;
import android.content.Context;
import android.content.Intent;
import android.util.DisplayMetrics;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.MotionEvent;
import android.view.WindowManager;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

/** Uses real InputDispatcher injection; run on an awake, unlocked physical display. */
@RunWith(AndroidJUnit4.class)
public class InputReadinessProbeTest {
 @Test public void dispatcherAcceptsFingerDownThenCancelWithoutClick(){
  Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();
  Context context=instrumentation.getTargetContext();
  Intent launch=context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
  launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
  context.startActivity(launch);
  instrumentation.waitForIdleSync();
  DisplayMetrics metrics=new DisplayMetrics();
  ((WindowManager)context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(metrics);
  UiAutomation automation=instrumentation.getUiAutomation();
  MotionEvent[] observed=new MotionEvent[2];
  try{
   AndroidInputInjector.probeTouch(metrics.widthPixels,metrics.heightPixels,event->{
    MotionEvent motion=(MotionEvent)event;
    try{
     observed[motion.getActionMasked()==MotionEvent.ACTION_DOWN?0:1]=MotionEvent.obtain(motion);
     return automation.injectInputEvent(motion,true);
    }finally{motion.recycle();}
   });
   assertNotNull(observed[0]);assertNotNull(observed[1]);
   assertEquals(MotionEvent.ACTION_DOWN,observed[0].getActionMasked());
   assertEquals(MotionEvent.ACTION_CANCEL,observed[1].getActionMasked());
   assertEquals(observed[0].getDownTime(),observed[1].getDownTime());
   for(MotionEvent motion:observed){
    assertEquals(KeyCharacterMap.VIRTUAL_KEYBOARD,motion.getDeviceId());
    assertEquals(InputDevice.SOURCE_TOUCHSCREEN,motion.getSource());
    assertEquals(MotionEvent.TOOL_TYPE_FINGER,motion.getToolType(0));
    assertEquals((metrics.widthPixels-1)/2f,motion.getX(),0f);
    assertEquals((metrics.heightPixels-1)/2f,motion.getY(),0f);
   }
  }finally{for(MotionEvent motion:observed)if(motion!=null)motion.recycle();}
 }
 @Test public void hoverProbeAlwaysExitsWithMatchingIdentity(){
  for(int tool:new int[]{MotionEvent.TOOL_TYPE_MOUSE,MotionEvent.TOOL_TYPE_STYLUS}){
   int source=tool==MotionEvent.TOOL_TYPE_MOUSE?InputDevice.SOURCE_MOUSE:InputDevice.SOURCE_STYLUS;
   for(int failureAction:new int[]{-1,MotionEvent.ACTION_HOVER_ENTER,MotionEvent.ACTION_HOVER_MOVE}){
    for(boolean throwing:new boolean[]{false,true}){
     java.util.ArrayList<MotionEvent> observed=new java.util.ArrayList<>();
     boolean rejected=false;
     try{
      AndroidInputInjector.probeHover(tool,source,event->{
       MotionEvent motion=(MotionEvent)event;
       try{
        observed.add(MotionEvent.obtain(motion));
        if(motion.getActionMasked()==failureAction){if(throwing)throw new IllegalStateException("Probe failure");return false;}
        return true;
       }finally{motion.recycle();}
      });
     }catch(IllegalStateException expected){rejected=true;}
     try{
      assertEquals(failureAction!=-1,rejected);
      assertEquals(MotionEvent.ACTION_HOVER_ENTER,observed.get(0).getActionMasked());
      assertEquals(MotionEvent.ACTION_HOVER_EXIT,observed.get(observed.size()-1).getActionMasked());
      assertEquals(failureAction==MotionEvent.ACTION_HOVER_ENTER?2:3,observed.size());
      for(MotionEvent motion:observed){
       assertEquals(KeyCharacterMap.VIRTUAL_KEYBOARD,motion.getDeviceId());
       assertEquals(tool,motion.getToolType(0));assertEquals(source,motion.getSource());
       assertEquals(observed.get(0).getPointerId(0),motion.getPointerId(0));
       assertEquals(observed.get(0).getDownTime(),motion.getDownTime());
      }
     }finally{for(MotionEvent motion:observed)motion.recycle();}
    }
   }
  }
 }
 @Test public void rejectedHoverExitCannotReportReadiness(){
  try{
   AndroidInputInjector.probeHover(MotionEvent.TOOL_TYPE_MOUSE,InputDevice.SOURCE_MOUSE,event->{
    MotionEvent motion=(MotionEvent)event;
    try{return motion.getActionMasked()!=MotionEvent.ACTION_HOVER_EXIT;}finally{motion.recycle();}
   });
   org.junit.Assert.fail("Rejected hover cleanup must fail readiness");
  }catch(IllegalStateException expected){assertEquals("Input readiness hover cleanup failed",expected.getMessage());}
 }
}
