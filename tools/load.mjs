import fs from 'node:fs';
import os from 'node:os';
import {gzipSync} from 'node:zlib';
import path from 'node:path';
import {performance} from 'node:perf_hooks';
import {Client} from './wire.mjs';
import {directory,cleanup,start,delay} from './demo-environment.mjs';
const [executable,output,...args]=process.argv.slice(2);
if(!executable||!output)throw Error('usage: node tools/load.mjs SERVER OUTPUT_DIRECTORY [--duration-ms N --repetitions N --rates CSV --durabilities CSV]');
const options={'duration-ms':'10000',repetitions:'3',rates:'100,1000,10000,100000',durabilities:'buffered,sync'};
for(let i=0;i<args.length;i+=2){const key=args[i].slice(2);if(!args[i].startsWith('--')||!(key in options)||args[i+1]===undefined)throw Error('invalid load option');options[key]=args[i+1];}
const duration=Number(options['duration-ms']),repetitions=Number(options.repetitions),rates=options.rates.split(',').map(Number),durabilities=options.durabilities.split(',');
if(!Number.isSafeInteger(duration)||duration<100||duration>300000||!Number.isSafeInteger(repetitions)||repetitions<1||repetitions>20||
 rates.some(rate=>!Number.isSafeInteger(rate)||rate<1||rate>1000000||rate*duration/1000>2000000)||durabilities.some(mode=>!['buffered','sync'].includes(mode)))throw Error('invalid load budget');
fs.mkdirSync(output,{recursive:true});
const scenarios=rates.map(rate=>({rate,duration,burst:rate===100000}));
const results=[];
const stats=values=>{values.sort((a,b)=>a-b);const p=q=>values.length?values[Math.min(values.length-1,Math.ceil(values.length*q)-1)]:null;return {samples:values.length,p50:p(.5),p99:p(.99),p999:p(.999),max:p(1)};};
for(const durability of durabilities)for(const scenario of scenarios)for(let repetition=1;repetition<=repetitions;repetition++){
 const dir=directory();let server;const clients=[];const rows=[];const latencies=[],wireLatencies=[],lateness=[];
 let sent=0,dropped=0,completed=0,failed=0,next=0,active=0;const sequences=[0,0,0],lastIds=[0,0,0],outcomes={};
 try{
  server=await start(executable,dir,['--durability',durability,'--timeout-ms','30000']);
  for(let i=0;i<3;i++){const c=await Client.connect(server.port,'127.0.0.1',30000);await c.login(i+1,String(i+1).repeat(32));clients.push(c);}
  // Bounded pipelining for load only: 64 unacknowledged commands per account. No automatic retries.
  // A bounded admission slot sheds requests explicitly instead of concealing overload in a closed loop.
  const busy=[0,0,0],pending=new Set();const begin=performance.now();
  const count=Math.floor(scenario.rate*scenario.duration/1000);
  const scheduled=i=>scenario.burst?Math.floor(i/10000)*100:1000*i/scenario.rate;
  const submit=(i,planned)=>{
   const account=i%3,actual=performance.now()-begin;
   if(busy[account]>=64){dropped++;rows.push([i,account+1,planned,actual,'','','shed']);return;}
   busy[account]++;active++;sent++;const sequence=++sequences[account],kind=sequence%2===1?1:2;
   const id=kind===1?1000+i:lastIds[account];if(kind===1)lastIds[account]=id;
   const promise=clients[account].submit(sequence,kind,id,1,kind===1?90:0,kind===1?1:0).then(reply=>{
    const done=performance.now()-begin;completed++;outcomes[reply.status]=(outcomes[reply.status]??0)+1;latencies.push((done-planned)*1000);wireLatencies.push((done-actual)*1000);lateness.push((actual-planned)*1000);
    rows.push([i,account+1,planned,actual,done,reply.status,'complete']);
   },error=>{failed++;rows.push([i,account+1,planned,actual,'',error.message,'failed']);}).finally(()=>{busy[account]--;active--;pending.delete(promise);});
   pending.add(promise);
  };
  while(next<count){const now=performance.now()-begin;let batch=0;
   while(next<count&&scheduled(next)<=now&&batch<2000){submit(next,scheduled(next));next++;batch++;}
   if(batch===2000)await new Promise(resolve=>setImmediate(resolve));else await delay(1);
  }
  await Promise.all(pending);const elapsed=performance.now()-begin;
  const stem=durability+'-'+scenario.rate+(scenario.burst?'-burst':'')+'-r'+repetition;
  fs.writeFileSync(path.join(output,stem+'.csv.gz'),gzipSync('id,account,scheduled_ms,submitted_ms,completed_ms,status,result\n'+rows.sort((a,b)=>a[0]-b[0]).map(r=>r.join(',')).join('\n')+'\n'));
  const bytes=fs.statSync(path.join(dir,'server.journal')).size;
  clients.forEach(c=>c.close());await server.stop();const recoveryStart=performance.now();server=await start(executable,dir,['--durability',durability]);
  const recoveryMs=performance.now()-recoveryStart;
  const result={durability,...scenario,repetition,outcomes,businessRejections:completed-(outcomes.accepted??0)-(outcomes.cancelled??0),offered:count,sent,shed:dropped,completed,failed,elapsedMs:elapsed,completedPerSecond:completed/(elapsed/1000),scheduledLatencyUs:stats(latencies),socketLatencyUs:stats(wireLatencies),generatorLatenessUs:stats(lateness),journalBytes:bytes,restartToReadyMs:recoveryMs,replayedSequence:server.ready.sequence,raw:stem+'.csv.gz',maxInFlightPerAccount:64};
  if(sent!==completed+failed||sent+dropped!==count||active!==0||server.ready.sequence!==completed||result.businessRejections!==0||failed!==0)throw Error('load accounting, controlled workload or recovery mismatch');
  results.push(result);fs.writeFileSync(path.join(output,'summary.json'),JSON.stringify(results,null,2)+'\n');console.log(JSON.stringify(result));
 }finally{clients.forEach(c=>c.close());await server?.stop();cleanup(dir);}
}
fs.writeFileSync(path.join(output,'environment.json'),JSON.stringify({date:new Date().toISOString(),platform:os.platform(),release:os.release(),cpu:os.cpus()[0].model,logicalCpus:os.cpus().length,totalMemory:os.totalmem(),node:process.version,notes:'Client and server share one unpinned host; no NIC, TLS, CPU isolation or power-loss simulation. Node scheduler lateness included. Three accounts, 64 in-flight requests per account; shed demand is reported, not a measured server saturation ceiling.'},null,2)+'\n');
fs.writeFileSync(path.join(output,'summary.json'),JSON.stringify(results,null,2)+'\n');
