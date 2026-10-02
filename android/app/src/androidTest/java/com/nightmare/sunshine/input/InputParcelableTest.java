package com.nightmare.sunshine.input;
import android.os.Parcel;
import org.junit.Test;
import org.junit.runner.RunWith;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import static org.junit.Assert.assertEquals;
@RunWith(AndroidJUnit4.class)
public class InputParcelableTest {
 @Test public void flaggedAndUnflaggedKeysRemainDistinctAcrossBinderParcel(){
  InputEventData plain=new InputEventData();plain.kind=InputEventData.KEY;plain.keyCode=13;
  InputEventData extended=new InputEventData(plain);extended.flags=1;extended.modifiers=16;
  Parcel parcel=Parcel.obtain();try{plain.writeToParcel(parcel,7);extended.writeToParcel(parcel,0);parcel.setDataPosition(0);InputEventData a=InputEventData.CREATOR.createFromParcel(parcel);InputEventData b=InputEventData.CREATOR.createFromParcel(parcel);assertEquals(0,a.flags);assertEquals(1,b.flags);assertEquals(66,AndroidInputInjector.mapKey(a.keyCode,a.modifiers));assertEquals(160,AndroidInputInjector.mapKey(b.keyCode,b.modifiers));}finally{parcel.recycle();}
 }
}
