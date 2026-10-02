package com.nightmare.sunshine.input;
import android.os.Parcel;
import android.os.Parcelable;
public final class InputResult implements Parcelable {
public static final int OK=0, PERMISSION_DENIED=1, UNSUPPORTED=2, UNSUPPORTED_TEXT=3, DEVICE_UNAVAILABLE=4, SESSION_CLOSED=5, INVALID_ARGUMENT=6; public InputResult(int code,String message) { this.code=code; this.message=message; }
public int code;
public String message;
public InputResult() {}
public InputResult(InputResult o) { code=o.code;message=o.message; }
private InputResult(Parcel p) { code=p.readInt();message=p.readString(); }
public void writeToParcel(Parcel p,int flags) { p.writeInt(code);p.writeString(message); }
public int describeContents(){return 0;}
public static final Parcelable.Creator<InputResult> CREATOR=new Parcelable.Creator<InputResult>(){public InputResult createFromParcel(Parcel p){return new InputResult(p);} public InputResult[] newArray(int n){return new InputResult[n];}};
}
