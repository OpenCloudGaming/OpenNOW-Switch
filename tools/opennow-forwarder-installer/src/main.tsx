import { AppletType, HidNpadButton } from '@nx.js/constants';
import { installNsp } from '@nx.js/install-title';
import { NcmStorageId } from '@nx.js/ncm';
import { NACP } from '@tootallnate/nacp';
import { useEffect, useState } from 'react';
import { Rect, Text } from 'react-tela';
import { render } from 'react-tela/render';
import type { Module } from './hacbrewpack';
import { loadForwarderKeys } from './prod-keys';
import { deterministicTitleId } from './util';

const requestPath = 'sdmc:/switch/SwitchNOW/forwarder_request.json';
const resultPath = 'sdmc:/switch/SwitchNOW/forwarder_result.json';
let exitEnabled = false;
let plusReleasedAfterInstall = false;

function waitForPaint(): Promise<void> {
  return new Promise((resolve) => requestAnimationFrame(() => resolve()));
}

function keepInstallerAlive() {
  const rawButtons = navigator.getGamepads()[0]?.rawButtons ?? 0n;
  const plusPressed =
    (rawButtons & BigInt(HidNpadButton.Plus)) !== 0n;
  if (!exitEnabled) {
    plusReleasedAfterInstall = false;
  } else {
    if (!plusPressed) plusReleasedAfterInstall = true;
    if (plusReleasedAfterInstall && plusPressed) Switch.exit();
  }
  requestAnimationFrame(keepInstallerAlive);
}

// Keep a native animation handle active from first paint through completion.
// Without this, nx.js may stop after the install promise settles before React's
// final state update reaches the framebuffer.
requestAnimationFrame(keepInstallerAlive);

interface InstallRequest {
  version: 1;
  name: string;
  author: string;
  displayVersion: string;
  nroPath: string;
  args: string;
}

function readRequest(): InstallRequest {
  const data = Switch.readFileSync(requestPath);
  if (!data) throw new Error('OpenNOW forwarder request is missing.');
  const request = JSON.parse(new TextDecoder().decode(data)) as InstallRequest;
  if (
    request.version !== 1 ||
    !request.name ||
    !request.nroPath.startsWith('sdmc:/') ||
    !request.args.includes(request.nroPath) ||
    request.name.length > 511 ||
    request.args.length > 1800
  ) {
    throw new Error('OpenNOW forwarder request is invalid.');
  }
  return request;
}

async function generateNsp(request: InstallRequest): Promise<Uint8Array> {
  const wasm = Switch.readFileSync('romfs:/hacbrewpack.wasm');
  const main = Switch.readFileSync('romfs:/template/exefs/main');
  const mainNpdm = Switch.readFileSync('romfs:/template/exefs/main.npdm');
  if (!wasm || !main || !mainNpdm) {
    throw new Error('Forwarder generator resources are incomplete.');
  }

  const app = new Switch.Application(request.nroPath);
  if (!app.icon) throw new Error('The shortcut NRO has no embedded icon.');
  const icon = app.icon;
  const titleId = await deterministicTitleId(request.nroPath, request.args);
  const keys = loadForwarderKeys();
  const createModule = (await import('./hacbrewpack.js')).default;

  return await new Promise<Uint8Array>((resolve, reject) => {
    let module: Module;
    void createModule({
      noInitialRun: true,
      wasmBinary: wasm,
      preRun(instance: Module) {
        module = instance;
        const { FS } = instance;
        FS.writeFile('/keys.dat', keys);
        FS.mkdir('/exefs');
        FS.writeFile('/exefs/main', new Uint8Array(main));
        FS.writeFile('/exefs/main.npdm', new Uint8Array(mainNpdm));

        const nacp = new NACP();
        nacp.id = titleId;
        nacp.title = request.name;
        nacp.author = request.author || 'OpenCloudGaming';
        nacp.version = request.displayVersion || '1.0.0';
        nacp.startupUserAccount = 0;
        nacp.userAccountSaveDataSize = 0n;
        nacp.userAccountSaveDataJournalSize = 0n;

        FS.mkdir('/control');
        FS.writeFile('/control/control.nacp', new Uint8Array(nacp.buffer));
        FS.writeFile('/control/icon_AmericanEnglish.dat', new Uint8Array(icon));
        FS.mkdir('/romfs');
        FS.writeFile('/romfs/nextArgv', new TextEncoder().encode(request.args));
        FS.writeFile(
          '/romfs/nextNroPath',
          new TextEncoder().encode(request.nroPath),
        );
      },
      onRuntimeInitialized() {
        try {
          const exitCode = module.callMain([
            '--nopatchnacplogo',
            '--titleid',
            titleId,
            '--nologo',
            '--plaintext',
          ]);
          if (exitCode !== 0) {
            reject(new Error(`Forwarder generator exited with code ${exitCode}.`));
            return;
          }
          const name = module.FS
            .readdir('/hacbrewpack_nsp')
            .find((entry) => entry.endsWith('.nsp'));
          if (!name) throw new Error('Generated NSP could not be found.');
          resolve(module.FS.readFile(`/hacbrewpack_nsp/${name}`));
        } catch (error) {
          reject(error);
        }
      },
    }).catch(reject);
  });
}

function Installer() {
  const [heading, setHeading] = useState('Preparing HOME shortcut');
  const [detail, setDetail] = useState('Reading the request from OpenNOW...');
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    function writeResult(result: object) {
      try {
        Switch.writeFileSync(resultPath, JSON.stringify(result));
      } catch {
        // The on-screen result remains available if SD result logging fails.
      }
    }

    async function install() {
      if (Switch.appletType() !== AppletType.Application) {
        throw new Error(
          'Full-memory/application mode is required to install a forwarder.',
        );
      }
      const request = readRequest();
      setHeading(`Creating ${request.name}`);
      setDetail('Building the personalized Horizon application...');
      const nsp = await generateNsp(request);

      setHeading(`Installing ${request.name}`);
      setDetail('Writing the forwarder to SD storage. Do not power off.');
      for await (const _event of installNsp(
        new Blob([nsp]),
        NcmStorageId.SdCard,
      )) {
        // The installer validates and commits each content record in order.
      }

      writeResult({
        version: 1,
        success: true,
        name: request.name,
        installedAt: Date.now(),
      });
      Switch.removeSync(requestPath);
      setHeading(`${request.name} installed`);
      setDetail(
        'The NSP has been installed to the Horizon HOME screen.\n\n' +
          'Press + to exit.',
      );
      await waitForPaint();
      await waitForPaint();
      exitEnabled = true;
    }

    void install().catch(async (error: unknown) => {
      const message = error instanceof Error ? error.message : String(error);
      writeResult({ version: 1, success: false, error: message });
      setFailed(true);
      setHeading('Could not install the HOME shortcut');
      setDetail(`${message}\n\nPress + to close the installer.`);
      await waitForPaint();
      await waitForPaint();
      exitEnabled = true;
    });
  }, []);

  return (
    <>
      <Rect fill='#0a0c0f' height={720} width={1280} x={0} y={0} />
      <Text
        fill={failed ? '#ff6b6b' : '#58d98a'}
        fontSize={42}
        x={72}
        y={210}
      >
        {heading}
      </Text>
      {detail.split('\n').map((line, index) => (
        <Text
          key={`${index}-${line}`}
          fill='#f2f4f7'
          fontSize={25}
          x={72}
          y={290 + index * 38}
        >
          {line}
        </Text>
      ))}
      <Text fill='#9098a5' fontSize={18} x={72} y={630}>
        OpenNOW Forwarder Installer - SD storage
      </Text>
    </>
  );
}

render(<Installer />, screen);
