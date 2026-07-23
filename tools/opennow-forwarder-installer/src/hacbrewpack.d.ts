export interface Module {
  FS: {
    mkdir(path: string): void;
    writeFile(path: string, data: Uint8Array): void;
    readFile(path: string): Uint8Array;
    readdir(path: string): string[];
  };
  callMain(args: string[]): number;
}

export interface ModuleOptions {
  noInitialRun: boolean;
  wasmBinary: ArrayBuffer;
  print?(text: string): void;
  printErr?(text: string): void;
  preRun?(module: Module): void;
  onRuntimeInitialized?(): void;
}

export default function createModule(options: ModuleOptions): Promise<Module>;
