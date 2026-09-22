import {spawn} from 'node:child_process';
import {EventEmitter} from 'node:events';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
export const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
export function directory(){return fs.mkdtempSync(path.join(os.tmpdir(),'meridian-'));}
export function cleanup(dir){
 const absolute=fs.realpathSync(dir),prefix=fs.realpathSync(os.tmpdir())+path.sep+'meridian-';
 if(!absolute.startsWith(prefix))throw Error('unexpected test directory');
 for(const name of fs.readdirSync(dir))fs.unlinkSync(path.join(dir,name));fs.rmdirSync(dir);
}
export function accounts(dir){
 const file=path.join(dir,'accounts.conf');fs.writeFileSync(file,
  '1 '+ '1'.repeat(32)+' 1000000000 100000 10000 1000000000 1000000\n'+
  '2 '+ '2'.repeat(32)+' 1000000000 100000 10000 1000000000 1000000\n'+
  '3 '+ '3'.repeat(32)+' 1000000000 100000 10000 1000000000 1000000\n');return file;
}
export async function start(executable,dir,extra=[]){
 const env=Object.fromEntries(Object.entries(process.env).map(([k,v])=>[process.platform==='win32'?k.toUpperCase():k,v]));
 const child=spawn(executable,['--journal',path.join(dir,'server.journal'),'--accounts',accounts(dir),'--port','0',...extra],
  {env,windowsHide:true,stdio:['ignore','pipe','pipe']});
 const events=new EventEmitter(),messages=[];let buffer='',stderr='';
 const closed=new Promise(resolve=>child.once('exit',(code,signal)=>{resolve({code,signal});events.emit('exit');}));
 child.stdout.on('data',bytes=>{buffer+=bytes.toString();for(;;){const end=buffer.indexOf('\n');if(end<0)break;
  const line=buffer.slice(0,end).trim();buffer=buffer.slice(end+1);try{const item=JSON.parse(line);messages.push(item);events.emit('message',item);}catch{}}
 });
 child.stderr.on('data',bytes=>{stderr=(stderr+bytes).slice(-16384);});
 child.on('error',e=>{stderr+=e.message;events.emit('exit');});
 const waitFor=(type,timeout=15000)=>new Promise((resolve,reject)=>{
  const existing=messages.find(m=>m.type===type);if(existing){resolve(existing);return;}
  const finish=(error,value)=>{clearTimeout(timer);events.off('message',onMessage);events.off('exit',onExit);error?reject(error):resolve(value);};
  const onMessage=item=>{if(item.type===type)finish(null,item);};
  const onExit=()=>finish(Error('server exited before '+type+': '+stderr));
  const timer=setTimeout(()=>finish(Error('server timeout waiting for '+type+': '+stderr)),timeout);
  events.on('message',onMessage);events.on('exit',onExit);
  if(child.exitCode!==null||child.signalCode!==null)onExit();
 });
 const stop=async()=>{
  if(child.exitCode===null&&child.signalCode===null)child.kill('SIGKILL');
  let timer;try{await Promise.race([closed,new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('server did not exit')),5000);})]);}finally{clearTimeout(timer);}
 };
 try{const ready=await waitFor('ready');return{child,ready,port:ready.port,waitFor,stop,closed,get stderr(){return stderr;}};}
 catch(error){await stop();throw error;}
}
