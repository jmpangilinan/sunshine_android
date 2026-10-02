package com.nightmare.sunshine.input;
/** Upper-half IDs reserved for readiness probes, disjoint from native client IDs. */
final class ProbeSessions {
 private int next=Integer.MAX_VALUE;
 synchronized int next(){if(next<=0x40000000)throw new IllegalStateException("Readiness probe IDs exhausted");return next--;}
}
