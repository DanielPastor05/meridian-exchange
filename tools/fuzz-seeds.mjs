import fs from 'node:fs';
import path from 'node:path';
import {frame,T,request,hello} from './wire.mjs';
const dir=process.argv[2];if(!dir)throw Error('corpus directory required');fs.mkdirSync(dir,{recursive:true});
for(const [name,bytes]of Object.entries({header:frame(T.submit).subarray(0,16),order:request(1,1,100,1,100,10),cancel:request(2,2,100),hello:hello(1,'1'.repeat(32))}))fs.writeFileSync(path.join(dir,name),bytes);
