import UIKit

/// 主界面: "运行 hello.exe" 按钮 + 日志输出区
/// 点击按钮后通过 WineRunner (ObjC) 调用 wine_run_exe, 弹出 UIAlertController 展示日文消息
class MainViewController: UIViewController {
    private let textView = UITextView()
    private let runner = WineRunner()
    private let runButton = UIButton(type: .system)

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "luciswin — Phase 1"
        view.backgroundColor = .systemBackground

        setupUI()
        appendLog("luciswin 就绪。点击按钮运行 hello.exe。")
    }

    private func setupUI() {
        runButton.setTitle("运行 hello.exe", for: .normal)
        runButton.titleLabel?.font = .preferredFont(forTextStyle: .title2)
        runButton.addTarget(self, action: #selector(runTapped), for: .touchUpInside)

        textView.isEditable = false
        textView.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.layer.borderColor = UIColor.separator.cgColor
        textView.layer.borderWidth = 1
        textView.layer.cornerRadius = 8

        let stack = UIStackView(arrangedSubviews: [runButton, textView])
        stack.axis = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)

        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 16),
            stack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -16),
            stack.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -16),
            textView.heightAnchor.constraint(greaterThanOrEqualToConstant: 200)
        ])
    }

    @objc private func runTapped() {
        runButton.isEnabled = false
        appendLog("— — —")
        runner.runHelloExe { [weak self] exitCode, log in
            self?.appendLog(log)
            self?.appendLog("退出码: \(exitCode)")
            self?.runButton.isEnabled = true
        }
    }

    private func appendLog(_ text: String) {
        textView.text += text + "\n"
    }
}
