package com.nightmare.sunshine.input;
/** Absolute mode remains on Binder until reset so drag movement/buttons never split devices. */
final class MouseRoute {
 private boolean absolute;
 private int buttons;
 boolean useVirtual(boolean available){return available&&!absolute;}
 int beginAbsolute(){if(absolute)return 0;absolute=true;return buttons;}
 void button(int button,boolean release){int bit=1<<(button-1);buttons=release?buttons&~bit:buttons|bit;}
 void reset(){absolute=false;buttons=0;}
}
