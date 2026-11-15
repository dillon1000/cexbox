using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Cexbox
{
    sealed partial class App : Application
    {
        public App()
        {
            InitializeComponent();
            // Xbox puts apps in mouse mode by default: the controller drives a cursor and
            // the D-pad scrolls instead of sending keys. A terminal needs the keys.
            RequiresPointerMode = ApplicationRequiresPointerMode.WhenRequested;

            // Log anything that would otherwise close the app without a trace, and keep going.
            UnhandledException += (s, e) =>
            {
                AppLog.Write("unhandled exception: " + e.Exception);
                e.Handled = true;
            };
            System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (s, e) =>
            {
                AppLog.Write("unobserved task exception: " + e.Exception);
                e.SetObserved();
            };
        }

        protected override void OnLaunched(LaunchActivatedEventArgs e)
        {
            if (!(Window.Current.Content is Frame frame))
            {
                frame = new Frame();
                Window.Current.Content = frame;
            }
            if (frame.Content == null)
            {
                frame.Navigate(typeof(MainPage), e.Arguments);
            }
            Window.Current.Activate();
        }
    }
}
