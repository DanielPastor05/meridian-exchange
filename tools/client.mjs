import {Client,T,ints,values,json,decodeOutcome} from './wire.mjs';
const args=process.argv.slice(2),options={host:'127.0.0.1',port:'9000',account:'1',token:process.env.MERIDIAN_TOKEN};
while(args[0]?.startsWith('--')){const key=args.shift().slice(2);if(!(key in options)||!args.length)throw Error('unknown option');options[key]=args.shift();}
if(!options.token){console.error('Set MERIDIAN_TOKEN or pass --token. Usage: client.mjs [--port N --account N --token HEX] new SEQUENCE ID BUY|SELL PRICE QTY | cancel SEQUENCE ID | kill SEQUENCE | resume SEQUENCE | account | book | events AFTER | ping | metrics');process.exit(2);}
const client=await Client.connect(Number(options.port),options.host);
try{
 await client.login(options.account,options.token);
 const op=args.shift()??'account';let reply;
 if(op==='new'){
  if(args.length!==5||!['BUY','SELL'].includes(args[2]))throw Error('new needs SEQUENCE ID BUY|SELL PRICE QTY');
  console.log(json(await client.submit(args[0],1,args[1],args[2]==='BUY'?1:2,args[3],args[4])));
 }else if(op==='cancel'||op==='kill'||op==='resume'){
  if(args.length!==(op==='cancel'?2:1))throw Error('invalid command arguments');
  console.log(json(await client.submit(args[0],op==='cancel'?2:op==='kill'?3:4,args[1]??0)));
 }else{
  const type=T[op];if(!type)throw Error('unknown operation');
  if(op==='book')reply=await client.send(type,ints(args[0]??0,args[1]??0,args[2]??256));
  else if(op==='events')reply=await client.send(type,ints(args[0]??0,args[1]??256));
  else reply=await client.send(type);
  console.log(json({type:reply.type,fields:values(reply.payload)}));
 }
}finally{client.close();}
