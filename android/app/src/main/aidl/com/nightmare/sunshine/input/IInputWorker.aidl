package com.nightmare.sunshine.input;
import android.os.IBinder;
import com.nightmare.sunshine.input.InputCapabilities;
import com.nightmare.sunshine.input.InputEventData;
import com.nightmare.sunshine.input.InputResult;
interface IInputWorker {
 InputCapabilities openSession(int sessionId, IBinder owner, int displayId, int width, int height, int rotation) = 0;
 oneway void submitBatch(int sessionId, long sequence, in InputEventData[] events) = 1;
 InputResult submitText(int sessionId, long sequence, String text) = 2;
 InputResult createController(int sessionId, int controllerId) = 3;
 void removeController(int sessionId, int controllerId) = 4;
 void resetSession(int sessionId, long sequence) = 5;
 void closeSession(int sessionId) = 6;
 void destroy() = 16777114;
}
