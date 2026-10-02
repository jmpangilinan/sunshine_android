package com.nightmare.sunshine.input;
import android.os.Parcel;
import android.os.Parcelable;
public final class InputEventData implements Parcelable {
public static final int REL_MOUSE=1, ABS_MOUSE=2, MOUSE_BUTTON=3, SCROLL=4, KEY=5, TOUCH=6, PEN=7, CONTROLLER_STATE=8;
public int kind;
public int pointerId;
public int action;
public int button;
public int keyCode;
public int flags;
public int modifiers;
public int controllerId;
public int toolType;
public int penButtons;
public int rotation;
public int tilt;
public int controllerButtons;
public int leftTrigger;
public int rightTrigger;
public int leftStickX;
public int leftStickY;
public int rightStickX;
public int rightStickY;
public boolean release;
public float x;
public float y;
public float deltaX;
public float deltaY;
public float verticalScroll;
public float horizontalScroll;
public float pressure;
public float contactMajor;
public float contactMinor;
public InputEventData() {}
public InputEventData(InputEventData o) { kind=o.kind;pointerId=o.pointerId;action=o.action;button=o.button;keyCode=o.keyCode;flags=o.flags;modifiers=o.modifiers;controllerId=o.controllerId;toolType=o.toolType;penButtons=o.penButtons;rotation=o.rotation;tilt=o.tilt;controllerButtons=o.controllerButtons;leftTrigger=o.leftTrigger;rightTrigger=o.rightTrigger;leftStickX=o.leftStickX;leftStickY=o.leftStickY;rightStickX=o.rightStickX;rightStickY=o.rightStickY;release=o.release;x=o.x;y=o.y;deltaX=o.deltaX;deltaY=o.deltaY;verticalScroll=o.verticalScroll;horizontalScroll=o.horizontalScroll;pressure=o.pressure;contactMajor=o.contactMajor;contactMinor=o.contactMinor; }
private InputEventData(Parcel p) { kind=p.readInt();pointerId=p.readInt();action=p.readInt();button=p.readInt();keyCode=p.readInt();flags=p.readInt();modifiers=p.readInt();controllerId=p.readInt();toolType=p.readInt();penButtons=p.readInt();rotation=p.readInt();tilt=p.readInt();controllerButtons=p.readInt();leftTrigger=p.readInt();rightTrigger=p.readInt();leftStickX=p.readInt();leftStickY=p.readInt();rightStickX=p.readInt();rightStickY=p.readInt();release=p.readInt()!=0;x=p.readFloat();y=p.readFloat();deltaX=p.readFloat();deltaY=p.readFloat();verticalScroll=p.readFloat();horizontalScroll=p.readFloat();pressure=p.readFloat();contactMajor=p.readFloat();contactMinor=p.readFloat(); }
public void writeToParcel(Parcel p,int parcelFlags) { p.writeInt(kind);p.writeInt(pointerId);p.writeInt(action);p.writeInt(button);p.writeInt(keyCode);p.writeInt(this.flags);p.writeInt(modifiers);p.writeInt(controllerId);p.writeInt(toolType);p.writeInt(penButtons);p.writeInt(rotation);p.writeInt(tilt);p.writeInt(controllerButtons);p.writeInt(leftTrigger);p.writeInt(rightTrigger);p.writeInt(leftStickX);p.writeInt(leftStickY);p.writeInt(rightStickX);p.writeInt(rightStickY);p.writeInt(release?1:0);p.writeFloat(x);p.writeFloat(y);p.writeFloat(deltaX);p.writeFloat(deltaY);p.writeFloat(verticalScroll);p.writeFloat(horizontalScroll);p.writeFloat(pressure);p.writeFloat(contactMajor);p.writeFloat(contactMinor); }
public int describeContents(){return 0;}
public static final Parcelable.Creator<InputEventData> CREATOR=new Parcelable.Creator<InputEventData>(){public InputEventData createFromParcel(Parcel p){return new InputEventData(p);} public InputEventData[] newArray(int n){return new InputEventData[n];}};
}
