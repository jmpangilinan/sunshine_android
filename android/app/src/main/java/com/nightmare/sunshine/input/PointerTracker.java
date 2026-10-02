package com.nightmare.sunshine.input;
import java.util.LinkedHashMap;
/** Protocol IDs are not Android IDs; Android IDs survive removal of earlier pointers. */
final class PointerTracker {
 private final LinkedHashMap<Integer,Integer> slots=new LinkedHashMap<>();
 int down(int id){if(id<0||slots.containsKey(id)||slots.size()>=10)throw new IllegalArgumentException("Invalid pointer down");int slot=0;while(slots.containsValue(slot))slot++;slots.put(id,slot);return slot;}
 int slot(int id){Integer slot=slots.get(id);if(slot==null)throw new IllegalArgumentException("Pointer is not down");return slot;}
 void up(int id){slot(id);slots.remove(id);}
 void cancel(){slots.clear();}
 boolean contains(int id){return slots.containsKey(id);}
}
