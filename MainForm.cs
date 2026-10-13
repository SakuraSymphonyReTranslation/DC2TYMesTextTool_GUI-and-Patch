using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using DC2TYMesTextTool;

namespace DC2TYMesTextTool_GUI
{
    public partial class MainForm : Form
    {
        private MesToolProcessor _processor;
        private TextBox _logBox = null!;
        
        // Single File Controls
        private TextBox _txtSingleInput = null!;
        private TextBox _txtSingleOutput = null!;
        private NumericUpDown _numSingleWrap = null!;

        // Batch Controls
        private TextBox _txtBatchInput = null!;
        private TextBox _txtBatchOutput = null!;
        private TextBox _txtBatchMesInput = null!; // For Import: JSON folder is input, but we need original MES folder too
        private NumericUpDown _numBatchWrap = null!;

        public MainForm()
        {
            InitializeComponent();
            _processor = new MesToolProcessor();
            _processor.Log += LogMessage;
        }

        private void LogMessage(string message)
        {
            if (InvokeRequired)
            {
                Invoke(new Action<string>(LogMessage), message);
                return;
            }
            _logBox.AppendText(message + Environment.NewLine);
        }

        private void InitializeComponent()
        {
            this.Text = "DC2TY Mes Text Tool GUI (Da Capo 2 ~ To You - Side Episodes)";
            this.Size = new Size(600, 500);
            
            var tabControl = new TabControl { Dock = DockStyle.Top, Height = 300 };
            
            // --- Single File Tab ---
            var tabSingle = new TabPage("Single File");
            
            var lblInput = new Label { Text = "Input MES/JSON:", Top = 20, Left = 10, Width = 100 };
            _txtSingleInput = new TextBox { Top = 20, Left = 120, Width = 350 };
            var btnBrowseInput = new Button { Text = "...", Top = 20, Left = 480, Width = 30 };
            btnBrowseInput.Click += (s, e) => BrowseFile(_txtSingleInput);

            var lblOutput = new Label { Text = "Output JSON/MES:", Top = 50, Left = 10, Width = 100 };
            _txtSingleOutput = new TextBox { Top = 50, Left = 120, Width = 350 };
            var btnBrowseOutput = new Button { Text = "...", Top = 50, Left = 480, Width = 30 };
            btnBrowseOutput.Click += (s, e) => BrowseSaveFile(_txtSingleOutput);

            var lblWrap = new Label { Text = "Word Wrap Width:", Top = 90, Left = 10, Width = 100 };
            _numSingleWrap = new NumericUpDown { Top = 90, Left = 120, Width = 60, Minimum = 10, Maximum = 100, Value = 70 }; 

            var btnExport = new Button { Text = "Export (MES -> JSON)", Top = 130, Left = 20, Width = 150 };
            btnExport.Click += (s, e) => RunSingleExport();
            
            var btnImport = new Button { Text = "Import (JSON -> MES)", Top = 130, Left = 180, Width = 150 };
            btnImport.Click += (s, e) => RunSingleImport();

            tabSingle.Controls.AddRange(new Control[] { 
                lblInput, _txtSingleInput, btnBrowseInput, 
                lblOutput, _txtSingleOutput, btnBrowseOutput,
                lblWrap, _numSingleWrap,
                btnExport, btnImport 
            });

            // --- Batch Tab ---
            var tabBatch = new TabPage("Batch");

            var lblBatchJson = new Label { Text = "JSON Folder:", Top = 20, Left = 10, Width = 100 };
            _txtBatchInput = new TextBox { Top = 20, Left = 120, Width = 350 }; // JSON folder for import, MES folder for export
            var btnBrowseBatchInput = new Button { Text = "...", Top = 20, Left = 480, Width = 30 };
            btnBrowseBatchInput.Click += (s, e) => BrowseFolder(_txtBatchInput);

            var lblBatchMes = new Label { Text = "MES Folder:", Top = 50, Left = 10, Width = 100 };
            _txtBatchMesInput = new TextBox { Top = 50, Left = 120, Width = 350 };
            var btnBrowseBatchMes = new Button { Text = "...", Top = 50, Left = 480, Width = 30 };
            btnBrowseBatchMes.Click += (s, e) => BrowseFolder(_txtBatchMesInput);
            
            var lblBatchOutput = new Label { Text = "Output Folder:", Top = 80, Left = 10, Width = 100 };
            _txtBatchOutput = new TextBox { Top = 80, Left = 120, Width = 350 };
            var btnBrowseBatchOutput = new Button { Text = "...", Top = 80, Left = 480, Width = 30 };
            btnBrowseBatchOutput.Click += (s, e) => BrowseFolder(_txtBatchOutput);

            var lblBatchWrap = new Label { Text = "Word Wrap Width:", Top = 120, Left = 10, Width = 100 };
            _numBatchWrap = new NumericUpDown { Top = 120, Left = 120, Width = 60, Minimum = 10, Maximum = 100, Value = 70 };

            var btnBatchExport = new Button { Text = "Batch Export (MES -> JSON)", Top = 160, Left = 20, Width = 180 };
            btnBatchExport.Click += (s, e) => RunBatchExport();
            
            var btnBatchImport = new Button { Text = "Batch Import (JSON -> MES)", Top = 160, Left = 210, Width = 180 };
            btnBatchImport.Click += (s, e) => RunBatchImport();

            Control lblHint = new Label { Text = "Export: Select MES Folder in 'MES Folder' box.\nImport: Select JSON Folder and MES Folder.", Top = 200, Left = 20, Width = 400, AutoSize = true };

            tabBatch.Controls.AddRange(new Control[] { 
                lblBatchJson, _txtBatchInput, btnBrowseBatchInput,
                lblBatchMes, _txtBatchMesInput, btnBrowseBatchMes,
                lblBatchOutput, _txtBatchOutput, btnBrowseBatchOutput,
                lblBatchWrap, _numBatchWrap,
                btnBatchExport, btnBatchImport, lblHint
            });

            tabControl.TabPages.Add(tabSingle);
            tabControl.TabPages.Add(tabBatch);
            
            // --- Log Box ---
            _logBox = new TextBox { 
                Multiline = true, 
                Dock = DockStyle.Bottom, 
                Height = 150, 
                ScrollBars = ScrollBars.Vertical,
                ReadOnly = true 
            };

            this.Controls.Add(tabControl);
            this.Controls.Add(_logBox);
            
            // Allow drag and drop
            this.AllowDrop = true;
            this.DragEnter += MainForm_DragEnter;
            this.DragDrop += MainForm_DragDrop;
        }

        private void BrowseFile(TextBox target)
        {
            using var fd = new OpenFileDialog();
            if (fd.ShowDialog() == DialogResult.OK) target.Text = fd.FileName;
        }

        private void BrowseSaveFile(TextBox target)
        {
            using var fd = new SaveFileDialog();
            if (fd.ShowDialog() == DialogResult.OK) target.Text = fd.FileName;
        }

        private void BrowseFolder(TextBox target)
        {
            using var fd = new FolderBrowserDialog();
            if (fd.ShowDialog() == DialogResult.OK) target.Text = fd.SelectedPath;
        }

        private void RunSingleExport()
        {
            _logBox.Clear();
            string input = _txtSingleInput.Text;
            string output = _txtSingleOutput.Text;
            if (string.IsNullOrWhiteSpace(output)) output = Path.ChangeExtension(input, ".json");
            
            System.Threading.Tasks.Task.Run(() => _processor.ExportMesToJson(input, output));
        }

        private void RunSingleImport()
        {
            _logBox.Clear();
            string jsonPaths = _txtSingleInput.Text; 
            // Ideally we need two inputs for single import (JSON + Original MES)
            // But usually they are named similarly. 
            // Let's assume input is JSON, and we try to guess MES if not provided?
            // Or better, let's just ask user to pick JSON as input, and we guess MES, 
            // OR we use the 'MES Folder' field logic?
            // Simpler: Just prompt if extension is wrong.
            
            // Wait, for single import we need: JSON path AND Original MES path.
            // My UI only has one input.
            // Let's try to assume original MES is same name as JSON but .mes?
            
            string originalMes = Path.ChangeExtension(jsonPaths, ".mes");
            if (!File.Exists(originalMes))
            {
                // Try to ask finding it?
                // For now just error
                 LogMessage($"Error: Could not find original MES file at {originalMes}");
                 return;
            }

            string output = _txtSingleOutput.Text;
            if (string.IsNullOrWhiteSpace(output)) output = Path.Combine(Path.GetDirectoryName(originalMes) ?? ".", "new_" + Path.GetFileName(originalMes));

            int wrap = (int)_numSingleWrap.Value;
            
            System.Threading.Tasks.Task.Run(() => _processor.ImportJsonToMes(jsonPaths, originalMes, output, wrap));
        }

        private void RunBatchExport()
        {
            _logBox.Clear();
            // Expect MES folder in _txtBatchMesInput
            string input = _txtBatchMesInput.Text;
            string output = _txtBatchInput.Text; // Use JSON folder as output
            if (string.IsNullOrWhiteSpace(output)) output = Path.Combine(input, "json_output");
            
            System.Threading.Tasks.Task.Run(() => _processor.BatchExport(input, output));
        }

        private void RunBatchImport()
        {
            _logBox.Clear();
            string jsonFolder = _txtBatchInput.Text;
            string mesFolder = _txtBatchMesInput.Text;
            string output = _txtBatchOutput.Text;
            if (string.IsNullOrWhiteSpace(output)) output = Path.Combine(mesFolder, "mes_output");
             int wrap = (int)_numBatchWrap.Value;

            System.Threading.Tasks.Task.Run(() => _processor.BatchImport(jsonFolder, mesFolder, output, wrap));
        }
        
        private void MainForm_DragEnter(object? sender, DragEventArgs e)
        {
            if (e.Data?.GetDataPresent(DataFormats.FileDrop) == true)
                e.Effect = DragDropEffects.Copy;
        }

        private void MainForm_DragDrop(object? sender, DragEventArgs e)
        {
            if (e.Data?.GetData(DataFormats.FileDrop) is not string[] files || files.Length == 0)
                return;

            if (Directory.Exists(files[0]))
            {
                _txtBatchMesInput.Text = files[0];
                // Smart guess: if folder has json, put in JSON input
                if (Directory.GetFiles(files[0], "*.json").Length > 0)
                     _txtBatchInput.Text = files[0];
            }
            else
            {
                _txtSingleInput.Text = files[0];
            }
        }
    }
}
