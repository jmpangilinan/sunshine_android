package com.nightmare.sunshine.input;
import android.os.Parcel;
import android.os.Parcelable;
public final class InputCapabilities implements Parcelable {
public static final int ROOT=1, SHIZUKU=2;
public int uid;
public int backendKind;
public int supportedEventMask;
public int maxControllers;
public boolean relativeMouse;
public boolean multitouch;
public boolean pen;
public boolean mappedText;
public boolean controllerRumble;
public boolean controllerMotion;
public boolean controllerTouch;
public boolean controllerBattery;
public String failureReason;
public InputCapabilities() {}
public InputCapabilities(InputCapabilities o) { uid=o.uid;backendKind=o.backendKind;supportedEventMask=o.supportedEventMask;maxControllers=o.maxControllers;relativeMouse=o.relativeMouse;multitouch=o.multitouch;pen=o.pen;mappedText=o.mappedText;controllerRumble=o.controllerRumble;controllerMotion=o.controllerMotion;controllerTouch=o.controllerTouch;controllerBattery=o.controllerBattery;failureReason=o.failureReason; }
private InputCapabilities(Parcel p) { uid=p.readInt();backendKind=p.readInt();supportedEventMask=p.readInt();maxControllers=p.readInt();relativeMouse=p.readInt()!=0;multitouch=p.readInt()!=0;pen=p.readInt()!=0;mappedText=p.readInt()!=0;controllerRumble=p.readInt()!=0;controllerMotion=p.readInt()!=0;controllerTouch=p.readInt()!=0;controllerBattery=p.readInt()!=0;failureReason=p.readString(); }
public void writeToParcel(Parcel p,int flags) { p.writeInt(uid);p.writeInt(backendKind);p.writeInt(supportedEventMask);p.writeInt(maxControllers);p.writeInt(relativeMouse?1:0);p.writeInt(multitouch?1:0);p.writeInt(pen?1:0);p.writeInt(mappedText?1:0);p.writeInt(controllerRumble?1:0);p.writeInt(controllerMotion?1:0);p.writeInt(controllerTouch?1:0);p.writeInt(controllerBattery?1:0);p.writeString(failureReason); }
public int describeContents(){return 0;}
public static final Parcelable.Creator<InputCapabilities> CREATOR=new Parcelable.Creator<InputCapabilities>(){public InputCapabilities createFromParcel(Parcel p){return new InputCapabilities(p);} public InputCapabilities[] newArray(int n){return new InputCapabilities[n];}};
}
