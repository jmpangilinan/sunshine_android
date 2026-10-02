package com.nightmare.sunshine.input;
import java.util.concurrent.ConcurrentHashMap;
/** Admission floors change at Binder entry, before queued work can execute. */
final class SequenceGate {
 private final ConcurrentHashMap<Integer,Long> floors=new ConcurrentHashMap<>();
 void open(int id){floors.put(id,0L);}
 boolean accepts(int id,long sequence){Long floor=floors.get(id);return floor!=null&&sequence>floor;}
 void reset(int id,long sequence){floors.computeIfPresent(id,(key,old)->Math.max(old,sequence));}
 void close(int id){floors.remove(id);}
 void clear(){floors.clear();}
}
